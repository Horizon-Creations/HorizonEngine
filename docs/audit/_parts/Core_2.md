# Core Slice 2 – ContentManager, Crypto, DebugDraw, Diagnostics

## Zusammenfassung

Der Slice (ContentManager/HAsset/EngineDependencies/TextureArrayBuild, Sha256/X25519, DebugDraw, CrashHandler/DevStatsTile/EngineProfiler/GlobalState/Log) ist insgesamt sorgfältig geschrieben: Async-Streaming nutzt Sink-`shared_ptr`, Cancel-Tokens, Zeit-Budget; Log und Profiler haben saubere Hot-Path-Guards. Die größten Schwächen sind (1) von Hand gepflegte Listen aller 26 `SlotMap`-Pools, die an vier Stellen auseinanderlaufen (echte Bugs), (2) Verzeichnis-Walks, die komplette Asset-Dateien lesen bzw. pro Datei `fs::relative` (= `weakly_canonical`) aufrufen, (3) ein Use-after-return in `EngineDeps::resolve` beim Abbruch, (4) ein 500-Zeilen-`parseAndRegisterAsset` mit mehrfachen Payload-Kopien und Material-Codegen im Ladepfad, (5) synchrones `fflush` + Konsole + Sinks unter globalem Log-Mutex. Funde: 4 Hoch, 9 Mittel, 7 Niedrig. Keine Messdaten vorhanden; Schätzungen sind als solche markiert.

---

### CORE-201 – Pool-Listen in `unloadAsset`, `forgetProjectContent`, `retargetAssetReferencesInMemory` sind unvollständig
- **ID**: CORE-201
- **Kategorie**: A (handgepflegte Redundanz; führt zu Fehlverhalten, auch C: Leak)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (behebt Bugs)
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2732-2743`, `3281-3294`, `3030-3044`; Pools deklariert in `include/ContentManager/ContentManager.h:789-814`
- **Problem**: Die Klasse hat 26 `SlotMap`-Member; drei Funktionen zählen sie von Hand auf (der Kommentar bei 2727 sagt selbst „MUST list every SlotMap member“). Abgleich gegen die Member:
  - `unloadAsset` kennt **nicht**: `m_structTypeAssets`, `m_enumTypeAssets`, `m_saveTemplateAssets`, `m_themeAssets`. Folge: `unloadAsset()` liefert für diese Typen `false`; `pollHotReload` kann sie nie neu laden (siehe CORE-204: `loadAsset` gibt dann die alte UUID zurück, der mtime wird nicht aktualisiert, jede Poll-Runde meldet die Datei erneut als „geändert“).
  - `forgetProjectContent` leert **nicht**: `m_themeAssets`, `m_boneMaskAssets`, `m_blendSpaceAssets`. Nach Projektwechsel bleiben die Daten des alten Projekts im Pool liegen (Leak, die Handle-Maps sind aber geleert, die Objekte sind unerreichbar).
  - `retargetAssetReferencesInMemory` (`rekeyAssetPaths`) kennt **nicht**: struct, enum, saveTemplate, theme, boneMask, blendSpace. Ein umbenanntes, geladenes Asset dieser Typen behält den alten `path`; `saveAsset` schriebe an den verlassenen Ort (genau das Szenario, das der Kommentar bei 3003-3007 verhindern will).
- **Code-Auszug**:
```cpp
const bool removed =
    tryRemove(m_staticMeshAssets)   || ... || tryRemove(m_blendSpaceAssets)   || tryRemove(m_sequenceAssets);
// fehlt: m_structTypeAssets, m_enumTypeAssets, m_saveTemplateAssets, m_themeAssets
```
- **Lösung**: Eine einzige Aufzählung der Pools (z. B. `template<class F> void forEachPool(F&& f)` mit `f(m_staticMeshAssets); f(...)`), und alle drei Stellen (plus `getXxx`/`register` falls gewünscht) darüber implementieren. `unloadAsset` kann zusätzlich den Pool über `m_assetTypeIndex[id]` per `switch` direkt wählen (statt 22 sequenziellen `get()`-Versuchen). Neue Pools müssen dann nur an einer Stelle eingetragen werden; ein `static_assert` auf die Poolanzahl verhindert Drift.
- **Erwarteter Effekt**: Korrektheit (Hot-Reload/Unload/Rename/Projektwechsel für 3-6 Typen); `unloadAsset` O(1) Pool-Wahl statt bis zu 22 Versuchen (vernachlässigbar). Kein Performance-Gewinn zu erwarten.
- **Risiko / Verifikation**: Verhalten ändert sich bewusst nur dort, wo es bisher falsch war. Tests ergänzen: Struct/Enum/Theme laden → `unloadAsset` ist `true`; `setContentRoot` auf anderes Projekt → `m_themeAssets` leer; `retargetAssetReferencesInMemory` aktualisiert `path` eines geladenen BoneMask/BlendSpace. Vorhandene `tests/` für ContentManager (Retarget/Hot-Reload) laufen lassen.

---

### CORE-202 – `discoverAssets` liest für jede `.hasset` den **gesamten** Dateiinhalt, nur um den Typ zu prüfen
- **ID**: CORE-202
- **Kategorie**: D (+ E: blockierende I/O)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2821-2833`; `include/ContentManager/HAsset.h:351-357, 380-434`; Aufrufer `src/HE_Scene/src/PlayerHost.cpp:37,57,80`, `src/HE_Core/src/Types/TypeRegistry.cpp:704-706`
- **Problem**: Der Loose-Content-Walk öffnet jede Datei mit `HAsset::Reader::open()`, das **alle Chunk-Payloads** (Meshes, Texturen, Audio, hunderte MB) in Vektoren liest, und verwirft sie, falls der Typ nicht passt. Der Kommentar in `HAsset.h:351-356` warnt genau davor und bietet `readAssetTypeFromFile` an – das hier nicht benutzt wird. `PlayerHost` ruft `discoverAssets` dreimal beim Start, `TypeRegistry::refreshFromContent` zweimal: bis zu 5 komplette Lesedurchläufe über den ganzen Content-Baum. Zusätzlich `std::filesystem::relative` pro Treffer (siehe CORE-205). Das Ergebnis ist außerdem mit `m_diskRegistry` (Kap. `scanContentDirectory`, enthält UUID → Pfad) bzw. `m_assetTypeIndex` großteils bereits bekannt.
- **Code-Auszug**:
```cpp
HAsset::Reader r;
if (!r.open(it->path().string()) ||
    r.assetType() != static_cast<uint16_t>(type)) continue;
```
- **Lösung**: (a) Minimal: `HAsset::readAssetTypeFromFile(path, t)` verwenden (32 Byte lesen), nur bei Treffer `loadAsset(rel)`. (b) Besser: den Typ beim `scanDirInto` mitspeichern (der Header wird dort ohnehin gelesen: `hdr.asset_type`) – `m_diskRegistry` bekommt `{path, type}`, `discoverAssets` iteriert die Registry statt das Dateisystem. Bereits geladene Assets werden über `loadAsset` → `isLoaded(path)` ohnehin kurzgeschlossen.
```cpp
uint16_t t; if (!HAsset::readAssetTypeFromFile(it->path().string(), t) || t != uint16_t(type)) continue;
```
- **Erwarteter Effekt (Schätzung)**: Start-/Play-Stall und Struct/Enum-Refresh von „Content-Größe lesen × 5“ auf „Dateizahl × 32 Byte × 5“; bei Projekten mit hunderten MB Content Sekunden → Millisekunden. Messen: Zeit von `PlayerHost`-Discovery mit großem Projekt.
- **Risiko / Verifikation**: (a) verhaltensgleich. (b) ändert die Reihenfolge nicht, wenn nach Pfad sortiert wird; Test: `discoverAssets` vor/nach liefert dieselbe UUID-Menge (loses Content, Engine-Content-Override, Pak-Fall). Registry-Variante deckt Dateien ab, die nach dem letzten Scan hinzukamen, nicht ab → entweder (a) wählen oder vorher `scanContentDirectory()`.

---

### CORE-203 – Use-after-return in `EngineDeps::resolve` beim Abbruch eines Downloads
- **ID**: CORE-203
- **Kategorie**: E (Nebenläufigkeit/Lifetime)
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (behebt Bug)
- **Ort**: `src/HE_Core/src/ContentManager/EngineDependencies.cpp:291-325`
- **Problem**: Die Fetch-Callbacks fangen `m`, `cv`, `finished`, `ok` (Stack-Variablen des `if`-Blocks) per `[&, i]` ein. Bei `options.cancelled()` wird mit `return w.result;` ausgestiegen, während „die bereits angestoßenen Downloads weiterlaufen“ (Kommentar 320). Wenn ein Download später fertig wird, schreibt sein Callback in `ok[i]`, `finished` und lockt/benachrichtigt `m`/`cv` – alles bereits zerstört (Stack/Heap freigegeben). Stack-Korruption bzw. UB auf dem SFTP-Worker.
- **Code-Auszug**:
```cpp
options.fetch(toFetch[i].substr(kEnginePrefixLen), [&, i](bool success)
{
    std::lock_guard<std::mutex> lock(m);
    ok[i] = success ? 1 : 0; ++finished; cv.notify_all();
});
...
if (options.cancelled && options.cancelled()) { w.result.cancelled = true; return w.result; }
```
- **Lösung**: Gemeinsamen Zustand in ein `struct State { std::mutex m; std::condition_variable cv; size_t finished = 0; std::vector<char> ok; };` legen, per `std::make_shared` allokieren und in jeder Lambda **by value** (shared_ptr) fangen. Außerdem die 100-ms-Poll-Schleife auf `cv.wait_for(lock, 100ms, pred)` mit Prädikat umstellen (entfernt das umständliche unlock/lock-Muster 314-316).
- **Erwarteter Effekt**: Beseitigt UB; kein Performance-Effekt.
- **Risiko / Verifikation**: Gering. Test mit Fake-`fetch`, der nach dem Abbruch (verzögert auf anderem Thread) die Completion aufruft – unter ASan/TSan laufen lassen (vorhandene EngineDeps-Tests erweitern).

---

### CORE-204 – Hot-Reload: Self-Reload nach jedem `saveAsset`, endlose Falsch-Änderungen bei nicht entladbaren Typen
- **ID**: CORE-204
- **Kategorie**: D (wiederholte Arbeit) / E
- **Schweregrad**: Hoch
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2290-2321` (saveAsset), `2845-2881` (pollHotReload), Aufruf `src/HE_Editor/EditorApplication.cpp:3430-3435`
- **Problem**: `saveAsset` schreibt die Datei, aktualisiert aber `m_pathMtime[path]` **nicht**. Der nächste `pollHotReload` (alle 1,5 s) sieht eine neuere Datei-mtime, entlädt das gerade gespeicherte Asset und lädt es erneut – jeder Save löst also einen vollen Reload aus (inkl. Re-Parse, bei Materialien Codegen, `InvalidateMaterial`, bei Texturen `InvalidateMaterial` für **alle** Materialien in `EditorApplication`). Bei den in CORE-201 genannten Typen scheitert `unloadAsset`, `loadAsset(relPath)` gibt wegen `isLoaded(path)` sofort die alte UUID zurück und `m_pathMtime` bleibt alt → die Datei wird **bei jedem Poll** erneut als „geändert“ gemeldet.
- **Code-Auszug**:
```cpp
unloadAsset(pathIt->second);                 // Ergebnis ignoriert
const HE::UUID newId = loadAsset(relPath);   // isLoaded(path) → alte UUID
if (!(newId == HE::UUID{})) changed.push_back(newId);
```
- **Lösung**: (1) Nach erfolgreichem `writeAssetFile` in `saveAsset` die mtime der geschriebenen Datei in `m_pathMtime[asset.path]` eintragen (nur wenn der Pfad bereits geladen/registriert ist). (2) In `pollHotReload` den Rückgabewert von `unloadAsset` prüfen und bei `false` (gepinnt/unbekannt) `m_pathMtime[relPath] = mtime` setzen und `continue` – kein falsches `changed`.
- **Erwarteter Effekt (Schätzung)**: Entfällt ein kompletter Reload pro Save (bei Materialien mit Graph: JSON-Parse + Codegen + Renderer-Invalidierung), und endloses 1,5-s-Rauschen bei 4 Typen.
- **Risiko / Verifikation**: Mittel: Prüfen, ob Editor-Panels sich auf den Self-Reload verlassen (z. B. um Instanzen via `syncMaterialInstancesOf` neu abzuleiten – laut Kommentar in `EditorApplication.cpp` leitet der Material Editor Instanzen bei eigenen Edits selbst ab). Test: speichern → `pollHotReload()` liefert leere Liste; externe Änderung → liefert UUID genau einmal; Theme/Struct extern ändern → nicht in Dauerschleife.

---

### CORE-205 – `pollHotReload` macht alle 1,5 s O(n) Syscalls und Allokationen auf dem Main-Thread
- **ID**: CORE-205
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/ContentManager/ContentManager.cpp:2845-2881`, `983-1004` (`resolveAbsolutePath`)
- **Problem**: Pro Poll: Kopie aller Pfad-Strings aus `m_pathMtime` in einen Vektor; pro Pfad `resolveAbsolutePath` (String-Konkatenation; für `Engine/…` bis zu drei `std::filesystem::exists`-Syscalls) und dazu `last_write_time`. Bei einigen tausend geladenen Assets sind das 10-20k Syscalls alle 1,5 s im UI-Thread (Hitch). Engine-Default-Dateien ändern sich im Betrieb praktisch nie.
- **Code-Auszug**:
```cpp
std::vector<std::string> paths; paths.reserve(m_pathMtime.size());
for (const auto& [p, _] : m_pathMtime) paths.push_back(p);
for (const auto& relPath : paths) {
    const std::string fullPath = resolveAbsolutePath(relPath);
    const auto mtime = fs::last_write_time(fullPath, ec);
```
- **Lösung**: In `m_pathMtime` neben der mtime den aufgelösten Absolutpfad cachen (`struct { file_time_type mtime; std::string full; }`), so entfallen Kopie und `exists`-Kaskade; `Engine/`-Defaults (ohne Override) überspringen; oder Poll auf Round-Robin-Scheiben (z. B. 1/8 der Einträge pro Tick) bzw. Dateisystem-Watcher.
- **Erwarteter Effekt (Schätzung)**: Faktor 2-4 weniger Syscalls (Pfadauflösung entfällt), mit Round-Robin gleichmäßig verteilte Last. Messen: Dauer von `pollHotReload` bei 5k Assets.
- **Risiko / Verifikation**: Gecachter Pfad wird ungültig, wenn ein Override unter `Content/Engine/…` neu entsteht → bei `noteContentChanged()` (`m_contentEpoch`) Cache verwerfen. Latenz der Erkennung steigt bei Round-Robin (Dokumentieren). Test: Override anlegen/löschen wird erkannt.

---

### CORE-206 – `std::filesystem::relative` (= `weakly_canonical`) pro Datei im Scan und in Pfad-Helfern
- **ID**: CORE-206
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:3260` (`scanDirInto`), `1019`, `1028` (`isEngineDefaultPath`/`isEngineOverridePath`), `1038`, `1054`, `1060` (`toContentRelativePath`), `2829` (`discoverAssets`)
- **Problem**: `fs::relative(p, base)` ruft `weakly_canonical` auf beide Argumente (libstdc++/MSVC), das pro Pfadkomponente `status`-Syscalls macht. `scanDirInto` läuft für drei Wurzeln (Content, Engine, Cache) über jede `.hasset`; `isEngine*Path`/`toContentRelativePath` werden vom Editor (Content Browser, Drag/Drop) aufgerufen, ggf. pro Item/Frame (Aufrufstellen außerhalb dieses Slice – unsicher). Außerdem ruft `scanDirInto` in der Schleife zwei Mal `p.path().extension()` und öffnet jede Datei einzeln per `ifstream`.
- **Code-Auszug**:
```cpp
const auto rel = std::filesystem::relative(p.path(), root, ec);
if (!ec) m_diskRegistry[id] = pathPrefix + rel.generic_string();
```
- **Lösung**: Im rekursiven Iterator ist der Pfad bereits unterhalb `root`; `p.path().lexically_relative(root)` genügt (reine String-Operation). Für `toContentRelativePath`/`isEngine*` ebenfalls lexikalisch, nach einmaliger Normalisierung der Roots (`weakly_canonical` im Setter, nicht im Aufruf).
- **Erwarteter Effekt (Schätzung)**: `scanContentDirectory` bei tausenden Dateien um einen großen Teil schneller (Syscalls pro Datei: ~5-10 → 2 für open/read). Messen mit `strace -c`/Profiler-Scope um `scanContentDirectory`.
- **Risiko / Verifikation**: Unterschied bei Symlinks/`..`-Segmenten: `weakly_canonical` löst sie auf, `lexically_relative` nicht. Wurzeln beim Setzen kanonisieren; Test mit Symlink-Content-Ordner und Windows-Laufwerksbuchstaben-Groß/Kleinschreibung (Fall `isEngineOverridePath`).

---

### CORE-207 – `parseAndRegisterAsset`: 500-Zeilen-Switch mit mehrfach kopiertem Payload
- **ID**: CORE-207
- **Kategorie**: A + C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `ContentManager.cpp:171-696` (u. a. `289`, `405`, `514-516`, `531`, `548`); `HAsset.h:437-477` (`openData`); `ContentManager.cpp:3048-3067, 3373-3376`
- **Problem**: (A) Eine Funktion decodiert 26 Assettypen inline; zehn Typen wiederholen wortgleich `a.xxx.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size())`. (C) Payload wird auf dem Ladepfad mehrfach kopiert: Pak-Eintrag (`readEntry` Vektor) → `Reader::openData` kopiert jeden Chunk in einen eigenen Vektor → `parseAndRegisterAsset` kopiert erneut ins Asset (`a.data = c->data` für Textur-Pixel, Audio, Font, Prefab, HC-Komponenten; `readVec` für Mesh-Arrays). Für große Texturen/Meshes sind das 2-3 Vollkopien, der letzte Schritt auf dem Main-Thread (`pollAsyncResults`). Die Kopie im Worker (`splitChunks`) ist unkritisch, die im Main-Thread nicht.
- **Code-Auszug**:
```cpp
if (const auto* c = reader.findChunk(HAsset::CHUNK_PIXL)) a.data = c->data;   // Kopie
if (const auto* c = reader.findChunk(HAsset::CHUNK_OGGD)) { a.audioData = c->data; ... }
```
- **Lösung**: (1) `HAsset::Reader::takeChunk(id)` (liefert `std::vector<uint8_t>&&`, der Reader ist ein lokal besessenes Wegwerf-Objekt) und in Blob-Typen (`PIXL`, `OGGD`/`PCMD`, `FNTD`, `PFAB`, `HCCP`, `PSHD`-Eingang) verschieben statt kopieren. (2) Pro Typ eine `static bool parseXxx(const Reader&/Reader&, XxxAsset&)`-Funktion, die Text-Typen über `chunkToString(reader, id, out)` füllen; `parseAndRegisterAsset` wird zur Tabelle `type → (pool, parseFn)`. (3) Optional `openData(std::vector<uint8_t>&&)` mit Offset-Views statt Chunk-Kopien.
- **Erwarteter Effekt (Schätzung)**: Main-Thread-Zeit beim Registrieren großer Texturen/Audio um die Kopierkosten (memcpy-Bandbreite, ~1-2 GB/s effektiv inkl. Allokation) kleiner, d. h. bei 64-MB-Textur grob 30-60 ms weniger pro Asset; Peak-Speicher −1× Assetgröße. Codegröße −150 Zeilen.
- **Risiko / Verifikation**: `findChunk` ist `const`; Reader wird nach dem Parse nicht mehr benutzt (außer `readMeshSections`, nach den `take`s aufrufen oder Reihenfolge beachten). Round-Trip-Tests je Typ (Save → Load → Save, Bytes identisch) und `test_hpak`/Streaming-Tests ausführen. Chunk-Layout bleibt unverändert.

---

### CORE-208 – Material-Ladepfad: Graph wird bis zu dreimal geparst, Codegen läuft auf dem Main-Thread
- **ID**: CORE-208
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:673-691`, `788-819`, `1723-1733`, `1735-1776` (insb. `1760-1766`)
- **Problem**: Jedes registrierte Basis-Material mit `nodeGraphJson` (ohne `precompiledShaders`, also Editor/Dev) durchläuft `regenerateMaterialFromGraph`: JSON-Parse (`materialGraphFromJson`), `generateFragment`, Param-Neuaufbau. Sind Remote-Assets aktiv (`m_remoteByPath` nicht leer), kommen `graphFunctionsReady` (Parse #2) und `prefetchRemoteReferences` (Parse #3, nur für Funktionsnamen) hinzu. Das geschieht synchron in `pollAsyncResults` (Main-Thread; das Zeitbudget greift erst *nach* dem Asset) und bei jedem Hot-Reload. Der Zweck („Codegen-Upgrade ohne Resave“) bräuchte die Regeneration nur, wenn der Codegen sich seit dem Speichern geändert hat.
- **Code-Auszug**:
```cpp
else if (m && !m->nodeGraphJson.empty() && m->precompiledShaders.empty())
{
    if (graphFunctionsReady(m->nodeGraphJson))   // parse #2 (remote only)
        regenerateMaterialFromGraph(id);          // parse #1 + generateFragment
}
```
- **Lösung**: (a) Graph nur einmal parsen und das `MaterialGraph` an `graphFunctionsReady`, `prefetch…` und `regenerate…` durchreichen (oder FunctionCall-Pfade direkt aus dem bereits geparsten Graph lesen). (b) Generierte GLSL mit `hash(nodeGraphJson, kCodegenVersion)` markieren (im Asset im Speicher, optional später im MTRL-Tail) und die Regeneration überspringen, wenn die gespeicherte `customShaderFragGlsl` nachweislich aus derselben Codegen-Version stammt.
- **Erwarteter Effekt**: (a) sicher −2 JSON-Parses bei Remote-Setups; (b) potenziell der größte Teil der Material-Ladezeit im Editor. Zahlen **unsicher / messen** (Profiler-Scope um `regenerateMaterialFromGraph`, Projekt mit vielen Graph-Materialien).
- **Risiko / Verifikation**: (b) ändert das Tail-Format → laut `HAsset.h`-Kommentar nur Append erlaubt, und `HpakWriter` (MTRL-Branch) muss mitziehen; deshalb zuerst nur in-memory-Cache. Test: Material mit Graph laden → GLSL identisch zum bisherigen Ergebnis (Golden), Funktion nachladen → Regeneration läuft weiterhin.

---

### CORE-209 – Logging: `fflush` pro Record, Datei-, Konsolen- und Sink-Ausgabe unter dem globalen Mutex
- **ID**: CORE-209
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/Log.cpp:259-285` (`emitLine`), `663-681` (`writeV`)
- **Problem**: Jeder Log-Record, der den Filter passiert, hält `mutex()` während `fwrite` + `fputc` + **`fflush`** (ein `write`-Syscall je Zeile), `fprintf` auf stdout/stderr (unter Windows-Konsole teuer) und aller registrierten Sinks. Alle Threads (Job-Worker, Streaming, Main) serialisieren darauf; beim Laden (INFO/DEBUG je Asset, Shader-Compile usw.) blockiert I/O den Frame-Thread. Sinks laufen unter dem Lock – ein Sink, der selbst loggt, deadlockt (std::mutex nicht rekursiv).
- **Code-Auszug**:
```cpp
std::fwrite(line, 1, static_cast<size_t>(length), s_file);
std::fputc('\n', s_file);
std::fflush(s_file);          // pro Record
...
for (const SinkSlot& s : sinks()) if (s.fn) s.fn(rec, s.user);
```
- **Lösung**: `fflush` nur bei `Level >= Warning` (und in `flush()`/Crash-Handler, der den Ring ohnehin schreibt); für Debug/Info reicht gepuffertes Schreiben plus periodischer Flush (z. B. beim `setFrameNumber`-Tick oder 250-ms-Timer). Optional: Sinks aus einer Kopie der Sink-Liste außerhalb des Locks aufrufen (Snapshot unter Lock, dann Aufruf), Datei-I/O auf Hintergrund-Thread mit Ring (Ring existiert bereits).
- **Erwarteter Effekt (Schätzung)**: Pro Info-Zeile ein Syscall weniger (≈ 5-30 µs auf SSD, mehr unter AV-Scannern/Windows); bei Ladeszenen mit tausenden Zeilen im Bereich 10-100 ms. **Messen**: Zähler `messageCount` × Zeit im `emitLine`.
- **Risiko / Verifikation**: Absturz-Sicherheit: letzte Info-Zeilen können bei hartem Kill (SIGKILL/Stromausfall) fehlen; Crash-Handler gibt den Ring ohnehin aus (`CrashHandler.cpp:38-44`). Tests: `test_log*` (falls vorhanden) und manuelles Crash-Szenario prüfen. Das Verhalten „Warnung/Fehler sofort auf Platte“ bleibt erhalten.

---

### CORE-210 – `EngineProfiler`: Ring über `vector::erase(begin())`, Live-Puffer ebenso, pro Frame frische Allokation
- **ID**: CORE-210
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/EngineProfiler.cpp:292-299`, `315-317`, `321-325`, `271`; `include/Diagnostics/EngineProfiler.h:210`
- **Problem**: Mit gesetztem `maxFrames` wird bei jedem Frame der älteste `ProfFrameRecord` per `m_frames.erase(m_frames.begin())` entfernt – O(N) Move-Assignments von Records, die je zwei `std::vector` enthalten (nicht trivially copyable). Dasselbe für `m_frameMarks` und – auch ohne Aufnahme – `pushLive` (240 Elemente pro Frame, solange der Live-HUD/`DevStatsTile` an ist). Der Profiler soll messen, verfälscht aber die Messung proportional zu `maxFrames`. Zusätzlich `m_current = ProfFrameRecord{}` pro Frame: `scopes`/`gpuPasses` werden jedes Mal neu aufgebaut (Wachstum 0 → N Reallokationen), weil `m_current` beim Push in `m_frames` leergezogen wird. `liveSnapshot()` kopiert den Puffer by value.
- **Code-Auszug**:
```cpp
if (m_maxFrames != 0 && m_frames.size() >= m_maxFrames)
    m_frames.erase(m_frames.begin());   // ring: drop oldest
```
- **Lösung**: `std::deque` (oder fester Ringpuffer mit Index) für `m_frames`, `m_frameMarks`, `m_live`; `snapshot()` kopiert dann einmalig linearisiert. Für `m_current.scopes` vor jedem Frame `reserve(letzte Größe)`; bei `m_singleFrame`/`m_frames` Vektorkapazität wiederverwenden.
- **Erwarteter Effekt (Schätzung)**: pro Frame während der Aufnahme von O(maxFrames) auf O(1); für `maxFrames` ≈ 5000 grob 20-80 µs/Frame weniger Messrauschen. **Messen** mit `maxFrames` groß/klein.
- **Risiko / Verifikation**: `lastFrame()` gibt `&m_frames.back()` – bei `deque` weiterhin gültig. Tests für Profiler-Dump/Stats (`HE::Prof::*` aus `ProfilerStats.h`) laufen lassen; Ringverhalten (`frameCount <= maxFrames`) prüfen.

---

### CORE-211 – Profiler-Dump: ganze Ausgabe als `nlohmann::json` im Speicher, mehrfach kopiert, synchron auf dem Main-Thread
- **ID**: CORE-211
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EngineProfiler.cpp:229-233` (Stop im `beginFrame`), `603`, `760`, `794`, `804`, `835`, `727-735`, `847`
- **Problem**: `doStopDump` läuft im `beginFrame` des Frame-Threads und baut für bis zu 120 000 Timeline-Spans plus jeden Frame (mit je allen Scopes) ein `json`-Objekt (bei nlohmann pro Objekt ein `std::map`, je Span 4 Schlüssel). Mehrere Zuweisungen **kopieren** statt zu verschieben: `j["summary"] = summary;`, `j["frames"] = frames;`, `jf["cpu"] = scopes;`, `jf["gpu"] = gp;`, `summary["hitches"] = jh;`. Danach `j.dump(1, '\t')` als zusätzlicher String im Speicher und `ofstream`. Beim Stoppen steht der Frame-Thread dadurch Sekunden still, der Peak-Speicher ist ein Vielfaches der Rohdaten.
- **Code-Auszug**:
```cpp
j["summary"] = summary;       // Kopie des gesamten Summary-Baums
...
jf["cpu"] = scopes;           // pro Frame Kopie
...
j["frames"] = frames;         // Kopie des größten Teils
f << j.dump(1, '\t');
```
- **Lösung**: `std::move` an allen genannten Stellen; Dump auf einen Hintergrund-Thread verlagern (Daten per `std::move` übergeben: `m_frames`, `lanes` sind nach Stop ohnehin nicht mehr nötig – `dumpNow()` für den „Dump“-Button muss aber kopieren); Frames/Spans streamend schreiben (`nlohmann::json::dump` in Chunks oder ein einfacher Writer), statt den gesamten Baum zu halten.
- **Erwarteter Effekt (Schätzung)**: Peak-Speicher und Stop-Stall bei langen Captures grob halbiert allein durch Moves; mit Thread zusätzlich kein Frame-Stall. **Messen** bei 5000-Frame-Capture.
- **Risiko / Verifikation**: Ausgabeformat darf sich nicht ändern (Tools/Skripte, die `profile_*.json` lesen) → Golden-Vergleich mit einem kurzen Capture; Thread-Variante: `m_justDumped`/`m_lastDumpPath` müssen thread-sicher werden. Niedrigster Aufwand: nur die Moves.

---

### CORE-212 – Mbed-TLS-Pfad: `ensurePsaInit` ist nicht thread-sicher und meldet fälschlich `false`
- **ID**: CORE-212
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (behebt Bug)
- **Ort**: `src/HE_Core/src/Crypto/X25519.cpp:88-97`
- **Problem**: Zwei nicht-atomare `static bool`s: `initialized = true` wird **vor** `psa_crypto_init()` gesetzt. Ruft ein zweiter Thread (Handshake mehrerer Verbindungen) währenddessen `ensurePsaInit` auf, sieht er `initialized == true`, aber `ok == false` und der Handshake scheitert (oder Datenrennen auf `ok`). Betrifft nur Builds ohne OpenSSL.
- **Code-Auszug**:
```cpp
static bool initialized = false; static bool ok = false;
if (!initialized) { initialized = true; ok = (psa_crypto_init() == PSA_SUCCESS); }
return ok;
```
- **Lösung**: Function-local static mit Initialisierer: `static const bool ok = (psa_crypto_init() == PSA_SUCCESS); return ok;` (C++11 garantiert thread-sichere Einmal-Initialisierung; kürzer).
- **Erwarteter Effekt**: Korrektheit, −5 Zeilen.
- **Risiko / Verifikation**: Gering. KATs `tests/test_crypto_x25519.cpp` mit Mbed-Backend laufen lassen; optional TSan-Test mit zwei Threads.

---

### CORE-213 – `GlobalState`: Merge-Funktionen sind O(n²) und stat-en jede Remote-Datei bei jedem Refresh
- **ID**: CORE-213
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Diagnostics/GlobalState.cpp:672-673`, `690-691`, `730-732`, `786-796`, `829`
- **Problem**: `mergeOverrideInto` und `mergeManifestInto` suchen jeden Eintrag linear per String-Vergleich in `subfolders`/`files` (`f->name == leafName`). Bei einem Engine-Katalog mit tausenden Einträgen in wenigen Ordnern ist das O(n·m) String-Vergleiche pro Refresh. Zusätzlich `fs::is_regular_file(cachePath)` (Syscall) für jedes Manifest-Element, das lokal nicht existiert, bei **jedem** `refreshEngineFolder` (laut Header-Kommentar standardmäßig alle 60 s und bei jedem Create/Rename), plus Kopie des gesamten `m_engineRemoteAssets`-Vektors (829). Läuft laut Kommentar abseits des Frame-Threads (SFTP-Worker/Refresh) → Mittel statt Hoch.
- **Code-Auszug**:
```cpp
for (HE::File* f : parent->files)
    if (f->name == leafName) { existsLocally = true; break; }
...
const bool cached = fs::is_regular_file(cachePath, ec);
```
- **Lösung**: Pro Ordner beim ersten Zugriff ein `unordered_map<std::string_view, Folder*/File*>` (oder Sortierung + `lower_bound`) aufbauen; Cache-Existenz einmal pro Refresh über einen einzigen Verzeichnis-Walk der Cache-Wurzel in ein `unordered_set` von relativen Pfaden statt N einzelner `stat`s.
- **Erwarteter Effekt (Schätzung)**: Bei ~5000 Manifest-Einträgen von Millionen String-Vergleichen und 5000 Syscalls auf ~ein paar zehn Tausend Operationen. **Messen**: Dauer `refreshEngineFolder` mit großem Manifest.
- **Risiko / Verifikation**: Reihenfolge der Knoten ändert sich nicht (Sortierung am Ende, `sortFolderTree`). Tests: bestehende Content-Folder-Tests (`remoteOnly`/`isLocalCacheCopy`-Fälle).

---

### CORE-214 – `ContentManager`-Getter/Register: ca. 75 handgeschriebene Einzeiler-Wrapper
- **ID**: CORE-214
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ContentManager.cpp:2541-2585`, `ContentManager.h:129-187`, `2658-2685`
- **Problem**: Jeder Assettyp hat `getX`, `getXMutable`, `registerX`, je eine Zeile mit denselben zwei Argumenten (Handle-Map + Pool). Dazu kommt, dass `lookupAsset` zwei Hash-/Slot-Zugriffe macht und das `m_handleToUUID`-Handle über Pools hinweg nur durch die Nach-Prüfung `a->id == id` unterschieden wird. Die Boilerplate ist der Grund für die Drift in CORE-201.
- **Lösung**: Ein Trait `template<class T> SlotMap<T>& pool()` (Spezialisierung per Member-Zeiger-Tabelle) und `template<class T> const T* get(HE::UUID) const`; die benannten Methoden bleiben als dünne Inline-Wrapper oder per Makro-freier Liste. Mittel- bis langfristig Pool-Lookup über `m_assetTypeIndex` statt Nach-Prüfung.
- **Erwarteter Effekt**: −80..100 Zeilen, weniger Drift. Keine Laufzeitänderung erwartet.
- **Risiko / Verifikation**: API bleibt gleich (Header-ABI, `HE_API`-Klasse) – nur additiv. Compile-Test aller Aufrufer; keine Verhaltensänderung.

---

### CORE-215 – `unloadAsset`/`pathForId`/`setMaterialParam`: lineare Suchen in Maps und Param-Namen
- **ID**: CORE-215
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:2747-2748` (`unloadAsset`), `ContentManager.h:73-80` (`pathForId`), `ContentManager.cpp:2587-2616` (`setMaterialParam`/`getMaterialParam`)
- **Problem**: `unloadAsset` durchläuft `m_pathToUUID` komplett, um den Pfad zur UUID zu finden (O(n) pro Unload; beim Projektwechsel/Massenunload O(n²)); `pathForId` ebenso (Kommentar verlangt Caching beim Aufrufer). `setMaterialParam` (Scripting-API, potentiell pro Frame und Material) sucht den Parameternamen linear per String-Vergleich.
- **Code-Auszug**:
```cpp
for (auto pit = m_pathToUUID.begin(); pit != m_pathToUUID.end(); ++pit)
    if (pit->second == id) { m_pathMtime.erase(pit->first); m_pathToUUID.erase(pit); break; }
```
- **Lösung**: Der Pfad steht im Asset (`asset.path`, vor dem Entfernen lesen) → `m_pathToUUID.erase(path)` / `m_pathMtime.erase(path)` direkt (O(1)); Achtung `registerRuntimeAsset` mit leerem Pfad. `setMaterialParam`: Slot-Index vom Skript cachen lassen (Name → Index einmalig) oder bei ≤ 16 Parametern belassen (16×String-Compare ist klein) – nur ändern, wenn Profiler es zeigt.
- **Erwarteter Effekt**: Unload O(1); `setMaterialParam` **unsicher / messen**.
- **Risiko / Verifikation**: `m_pathToUUID` kann mehrere Pfade auf dieselbe UUID abbilden (`loadAsset` setzt `m_pathToUUID[relativePath] = rid` für Pak-Aliase, Zeile 1080-1085); die Schleife entfernt aktuell nur den ersten Treffer. Verhalten dort angleichen oder dokumentieren; Test: Alias-Pfad nach Unload.

---

### CORE-216 – Pak-Streaming: pro Asset-Job ein eigener Datei-Open und Pfad-Kopie
- **ID**: CORE-216
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `ContentManager.cpp:1625-1693` (`launchPakLoad`), `1785-1812` (`streamMountedAssets`)
- **Problem**: Jeder `AssetLoadPak`-Job erzeugt einen neuen `HpakReader`, öffnet die `.hpak`-Datei (`openShared`) und liest einen Eintrag. `streamMountedAssets` startet einen Job je Pak-Eintrag, d. h. N Datei-Opens und N Kopien von `path`, `key`, `std::shared_ptr<Toc>`. Der Kommentar zeigt, dass der quadratische TOC-Teil schon beseitigt wurde; Datei-Open pro Job bleibt.
- **Lösung**: Thread-lokalen Reader-Cache (`thread_local std::unique_ptr<HpakReader>` je Pfad+TOC-Zeiger) im Worker, der bei Pfadwechsel neu öffnet. Verhalten identisch.
- **Erwarteter Effekt**: **unsicher / messen** (Open-Kosten hängen vom Betriebssystem/AV ab; HpakReader selbst liegt außerhalb dieses Slice).
- **Risiko / Verifikation**: Lifetime des Readers vs. ausgetauschte Pak-Datei (die `reader.open(path)`-Fallback-Logik bleibt nötig). Messen mit `strace -c`/Process Monitor beim Start eines Spiels mit großem Pak.

---

### CORE-217 – `CrashHandler`: POSIX ohne Alternate-Stack und mit nicht signalsicheren Aufrufen; irreführender Kommentar unter Windows
- **ID**: CORE-217
- **Kategorie**: F / E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Diagnostics/CrashHandler.cpp:72-121`, `348-354`, `430-440`
- **Problem**: (1) Der POSIX-Handler läuft auf dem Stack des abstürzenden Threads (`sigaltstack` fehlt): bei Stack-Overflow-SIGSEGV kann er nichts ausgeben, obwohl der Windows-Zweig genau diesen Fall adressiert. (2) Er ruft `fopen`, `fprintf`, `localtime`, `ctime`, `backtrace_symbols` (malloc) auf – nicht async-signal-safe; bei Heap-Korruption (häufige Absturzursache) kann er selbst hängen. Der Kommentar bei 98-100 räumt das teilweise ein. (3) Windows-Kommentar sagt „the wait below is bounded rather than infinite“, der Code ist `for (;;) ::Sleep(1000);`.
- **Code-Auszug**:
```cpp
// A crash INSIDE the report thread lands here too, so the wait below is bounded rather than infinite.
if (::InterlockedCompareExchange(&s_reporting, 1, 0) != 0) { for (;;) ::Sleep(1000); }
```
- **Lösung**: `sigaltstack` mit statischem 64-KB-Puffer + `SA_ONSTACK`; Report-Pfad vorab bauen (`buildReportPath` im `install`, nicht im Handler), `open`/`write` statt `fopen`/`fprintf`, `backtrace_symbols_fd` statt `backtrace_symbols`. Kommentar anpassen oder tatsächlich begrenzen (z. B. `WaitForSingleObject(..., 60000)` Schleife).
- **Erwarteter Effekt**: Robustere Crash-Reports (Stack-Overflow, Heap-Korruption). Kein Laufzeiteffekt.
- **Risiko / Verifikation**: Gering; Test mit Absicht ausgelöstem SIGSEGV/Stack-Overflow auf Linux/macOS, Ausgabeformat bleibt (Header-Kommentare, Report-Zeilen) – von Bug-Reporter ggf. geparst (`Log::recentProblemLines`-Konsumenten prüfen).

---

### CORE-218 – `ProfileScope` ruft pro Konstruktion die exportierte `instance()`; „Zero cost“ stimmt nur annähernd
- **ID**: CORE-218
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `include/Diagnostics/EngineProfiler.h:334-349`, `EngineProfiler.cpp:154-158`
- **Problem**: Der Header verspricht „one bool check per scope“. Tatsächlich ruft jede `ProfileScope`-Konstruktion `EngineProfiler::instance()` (nicht inline, `HE_API`-exportiert, Function-Static-Guard) und dann `isRecording()` (atomares Acquire-Load) – auch im Normalbetrieb ohne Aufnahme. Der JobSystem-Pool umhüllt jeden Task mit einem solchen Scope, der Main-Loop hat viele (hunderte bis tausende pro Frame).
- **Code-Auszug**:
```cpp
explicit ProfileScope(const char* name) : m_opened(EngineProfiler::instance().isRecording())
```
- **Lösung**: Ein inline zugängliches `extern std::atomic<bool>`-Flag (`EngineProfiler::s_recordingFlag`, im .cpp definiert, im Header als `HE_API extern`) für den Schnellpfad; `instance()` nur noch bei `true`.
- **Erwarteter Effekt (Schätzung)**: wenige ns pro Scope; bei ~2000 Scopes/Frame ≈ 5-20 µs. Nur ändern, wenn Profiling es relevant zeigt (**messen**).
- **Risiko / Verifikation**: Gering; `m_recording` bleibt Single Source (Flag = Referenz auf dieselbe Variable).

---

### CORE-219 – `Sha256`: reine Softwareimplementierung für Startup-Integritätsprüfung
- **ID**: CORE-219
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Crypto/Sha256.cpp:39-67`, `134-152`; Aufrufer `src/HE_Core/src/Integrity/IntegrityProbe.cpp:103,128`
- **Problem**: Die Integritätsprobe hasht beim Start Executable und alle Engine-Bibliotheken („tens of megabytes each“, laut Header) mit einer skalaren Implementierung (~150-300 MB/s je Kern, ohne SHA-NI/ARMv8-SHA). Der Header begründet die Eigenimplementierung mit Abhängigkeitsfreiheit; die Krypto-Backends (OpenSSL/mbedTLS) stehen im selben Modul zur Verfügung (siehe `X25519.cpp`) und beschleunigen SHA-256 hardwarebasiert.
- **Lösung**: Bei vorhandenem Backend `EVP_Digest*` bzw. `psa_hash_*` verwenden und die Eigenimplementierung als Fallback behalten; alternativ Hashing in einem Hintergrund-Thread parallel zur restlichen Initialisierung. `finish()` allokiert nichts; `~Sha256() = default;` kann inline bleiben.
- **Erwarteter Effekt (Schätzung)**: bei 100-200 MB Binaries ~0,3-0,7 s → ~0,05-0,15 s Startzeit. **Messen**: `IntegrityProbe`-Dauer im Startlog.
- **Risiko / Verifikation**: Digest bleibt identisch (KATs in `test_hpak`/`test_net_secure`, siehe Header). Zwei Codepfade → beide testen.

---

### CORE-220 – Kleinkram: toter Code, Duplikate, Kommentare, schwerer Header
- **ID**: CORE-220
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: mehrere (siehe Liste)
- **Problem / Lösung** (je 1-5 Zeilen Änderung, rein lesbarkeitsbedingt):
  1. `EngineProfiler.h:191,295` / `EngineProfiler.cpp:247,308`: `m_forceDetailed` wird nirgends auf `true` gesetzt (Grep im gesamten `src/`), `detailedGpuCapture()` ist damit nur `m_detailedGpu`. Flag und die zugehörigen Kommentare entfernen.
  2. `ContentManager.cpp:3549-3551`: `((x % kCell) <= 1 && (x % kCell) >= 0)` – `>= 0` ist für `int` ≥ 0 immer wahr; `(x % kCell + y % kCell < 2)` allein deckt die Ecke ab.
  3. `ContentManager.cpp:3114-3181`: `mountPak` parst zwei gleich aufgebaute JSON-Indizes (Pfad-Index, Typ-Index) mit duplizierter `"hi:lo"`-Zerlegung inkl. `try/catch` um `stoull`; eine Hilfsfunktion `parseUuidPair(const std::string&, UUID&)` (mit `std::from_chars`, kein Exception-Pfad) genügt.
  4. `Log.cpp:908-929` (`systemInfoBlock`) und `946-956` (`logStartupBanner`) formatieren Build/Platform/CPU/Memory doppelt und rufen `osVersionString()`/`cpuBrandString()` (liest `/proc/cpuinfo`, `/etc/os-release`) jeweils neu; `logStartupBanner` könnte `systemInfoBlock()` zeilenweise loggen.
  5. `Log.cpp:97-108`: `verbosities()` hat zwei Function-Statics (Array + `seeded`) und wird in jedem `enabled()` – also in jedem `HE_LOG_*`-Aufruf, auch bei deaktiviertem Level – aufgerufen. Ein Namespace-Scope-Array mit `constexpr`-Konstruktor (Startwert `Info`) spart zwei Guard-Prüfungen im Hot Path.
  6. `GlobalState.h:11-13`: Der Header zieht `<nlohmann/json.hpp>` (schwer) und definiert globales `using json = nlohmann::json;`; nur 17 Includer, aber der Alias leakt in deren Namespace. JSON-Zugriff hinter `.cpp` verstecken (`m_customConfig` als `std::unique_ptr<json>`/Pimpl, `setCustomConfigEntry(const std::string&, const json&)` → Overloads für bool/int/float/string).
  7. `TextureArrayBuild.cpp:88-98`: `prev` kopiert Level 0 jeder Schicht zusätzlich zu `data.insert`, und `halve` alloziert pro Mip einen neuen Vektor; direktes Schreiben in `data` (Ziel-Offset per `textureArrayOffset`) vermeidet 1+mips Allokationen/Kopien pro Slice. Editor/Bake-Pfad, nur bei großen Arrays relevant.
  8. `DebugDraw.h:46-66`: `sphere`/`capsule` berechnen `cos/sin` pro Segment doppelt (a1 von Iteration i ist a0 von i+1); die Werte der Vorgängeriteration weiterreichen. Editor-only, `vector::push_back` ohne `reserve` – bei Zeichnung vieler Collider addiert sich das (zu messen).
- **Erwarteter Effekt**: −Code, kleine Einsparungen; keine funktionalen Änderungen.
- **Risiko / Verifikation**: Gering; 2, 3, 5 mit bestehenden Tests (`test_hpak`, Log-Tests) absichern; 6 per Compile aller 17 Includer.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/PlayerHost.cpp:37,57,80` ruft `ContentManager::discoverAssets` dreimal beim Spielstart (siehe CORE-202); `src/HE_Core/src/Types/TypeRegistry.cpp:704-706` zweimal. Eine gemeinsame, gecachte Typ-Liste würde die Wiederholung vermeiden.
- `src/HE_Editor/EditorApplication.cpp:3430-3500`: Reaktion auf `pollHotReload`: bei Textur-Änderung `InvalidateMaterial` für **jedes** Material – bei Self-Reload nach Save (CORE-204) unnötig teuer.
- `src/HE_Core/include/ContentManager/AssetRefRetarget.h` (nicht im Slice) wird mit denselben Pool-Listen konsistent sein müssen, falls dort ähnliche Aufzählungen existieren.
- `Hpak/HpakWriter.cpp` MTRL-Branch hängt am Feldlayout in `encodeAssetChunks` (Kommentar `ContentManager.cpp:1992-1997`); Änderungen an CORE-208(b) betreffen ihn.
- `Diagnostics/ProfilerStats.h` (`aggregateScopes`, `findHitches`, `computePercentiles`) wird in `dumpNow` für den gesamten Capture auf dem Frame-Thread berechnet – Teil des Stalls aus CORE-211, nicht gelesen.
- `Hpak/HpakReader` (`openShared`, `readEntry`) bestimmt die Wirkung von CORE-216; liegt außerhalb dieses Slice.

## Unsicher / Messung nötig

- CORE-205/206: Anzahl Syscalls und Dauer von `pollHotReload` / `scanContentDirectory` bei realen Projekten (5k+ Assets); Windows-AV kann Open-Kosten dominieren.
- CORE-207: tatsächlicher Anteil der Kopien an der Main-Thread-Zeit in `pollAsyncResults` (Profiler-Scope `ContentManager::load`).
- CORE-208: Anteil von `regenerateMaterialFromGraph` an der Editor-Ladezeit; wie oft alte Codegen-Stände wirklich neu erzeugt werden müssen.
- CORE-209: Log-Volumen pro Sekunde in Ladeszenen; Wirkung von `fflush` pro Zeile auf Windows/NTFS mit Echtzeit-Scanner.
- CORE-210/211: Auswirkung von `maxFrames` und Capture-Länge auf Frame-Zeit bzw. Stop-Stall.
- CORE-213: Größe des echten EngineContent-Katalogs (Einträge pro Ordner).
- CORE-216: Kosten eines `open()` je Pak-Job.
- `setMaterialParam` (CORE-215) und `ProfileScope` (CORE-218) nur ändern, wenn Profiling sie zeigt.
- Nicht verifiziert: ob `isEngineDefaultPath`/`toContentRelativePath` im Editor pro Frame aufgerufen werden (Aufrufstellen in `src/HE_Editor/` nicht gelesen).

## Gelesen

Ja, alle 23 Dateien des Slice 2 vollständig gelesen (`ContentManager.cpp` 1-3687 in Blöcken, `ContentManager.h`, `DefaultAssets.h`, `EngineDependencies.{h,cpp}`, `HAsset.h`, `TextureArrayBuild.{h,cpp}`, `Sha256.{h,cpp}`, `X25519.{h,cpp}`, `DebugDraw.h`, `CrashHandler.{h,cpp}`, `DevStatsTile.{h,cpp}`, `DiagnosticsStructs.h`, `EngineProfiler.{h,cpp}`, `GlobalState.{h,cpp}`, `Log.cpp`); außerhalb des Slice nur gezielte Greps (Log.h-Makros, `pollHotReload`/`discoverAssets`-Aufrufer, `IntegrityProbe`-Aufrufe).
