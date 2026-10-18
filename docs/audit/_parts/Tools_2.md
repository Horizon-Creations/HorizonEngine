# Tools Slice 2 – MatGen, MeshGen, Packer, ShaderCompiler, WeatherSoundGen, WidgetGen

## Zusammenfassung

Der Slice (11 Dateien, ca. 3470 Zeilen) ist insgesamt sauber und gut kommentiert. Die Generatoren (`mat_gen`, `mesh_gen`, `widget_gen`, `weather_sound_gen`) laufen offline und nicht im Build-Graph, daher ist ihre Performance praktisch irrelevant. Relevant für Ladezeit und Offline-Pipeline sind vor allem `he_shadercompiler` (wird zur Laufzeit von allen Renderern über `MaterialShaderLibrary` genutzt) und `hpak_packer` (CLI-Wrapper um `HpakWriter`). Funde: 1 Hoch, 3 Mittel, 6 Niedrig. Die Hauptpunkte sind das pro Aufruf wiederholte `glslang::InitializeProcess()/FinalizeProcess()`, der fehlende Inkrementell-Cache im Packer-CLI und ein Generator, der Verdrahtungsfehler nur auf stderr meldet und trotzdem Exit-Code 0 liefert.

## Funde

### TLS-201 – glslang-Prozess wird bei jedem Shader-Compile neu initialisiert und abgebaut
- **ID**: TLS-201
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:194-204, 206-258, 260-304, 306-329, 331-354
- **Problem**: Jede der fünf Einstiegsfunktionen ruft `glslang::InitializeProcess()` und am Ende `FinalizeProcess()`. glslang baut dabei seine Built-in-Symboltabellen (Funktions- und Variablentabellen je Stage/Profil) komplett neu auf. Diese Initialisierung ist der teuerste Teil kleiner Shader-Compiles. Der Kommentar "glslang's process init/teardown is global, not re-entrant — serialise it" begründet nur den Mutex, nicht, warum pro Aufruf neu initialisiert werden muss. glslang unterstützt Init einmal pro Prozess (intern referenzgezählt). Die Funktionen laufen zur Laufzeit, wenn `MaterialShaderLibrary` ein Material ohne vorkompilierte Variante cross-kompiliert (Fallback), sowie für Pinned-Varianten von SSR/Decals/Clustered-GL. Jeder Material-/Pass-Wechsel beim Laden zahlt also die Initialisierung erneut.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lock(g_glslangMutex);
glslang::InitializeProcess();
if (glslToSpirv(glsl, stage, r.spirv, r.log))
    r.ok = spirvToSource(r.spirv, target, r.source, r.log);
glslang::FinalizeProcess();
```
- **Lösung**: Einmalige Initialisierung per Function-Local-Static-Guard, ohne Finalize pro Aufruf:
  1. `struct GlslangProcess { GlslangProcess(){ glslang::InitializeProcess(); } ~GlslangProcess(){ glslang::FinalizeProcess(); } };`
  2. In einem gemeinsamen Helfer (siehe TLS-202) `static GlslangProcess proc;` anlegen, danach Mutex nehmen (der Mutex bleibt für `TShader`/`TProgram`-Nutzung vorerst bestehen, so ändert sich das Verhalten nicht).
  3. Optional später prüfen, ob der Mutex für das reine Parsen entfallen kann (glslang ist nach Init für getrennte TShader/TProgram-Instanzen thread-safe). Das ist ein eigener Schritt und nicht Teil dieses Fundes.
- **Erwarteter Effekt**: Schätzung: pro Compile ein- bis zweistellige Millisekunden gespart (Symboltabellen-Aufbau). Bei vielen Material-Varianten und Pinned-Shadern beim Laden ergibt das spürbare Ladezeit. Ohne Messung nicht belegt.
- **Risiko / Verifikation**: Statisches Destruktor-Timing beim Prozessende (Finalize nach Renderer-Teardown ist unkritisch, da kein glslang-Objekt überlebt). Offline-Tools, die `compile` aus mehreren Threads nutzen, bleiben durch den Mutex serialisiert. Messen mit `compileMany` des Spike (`spike_main.cpp`) in einer Schleife mit 100 Aufrufen vor/nach der Änderung; Material-Tests (`tests/` mit Shaderc, z. B. `grep -l shaderc tests/*.cpp`) laufen lassen.

### TLS-202 – Fünf nahezu identische Compile-Pipelines mit kopiertem Rahmen
- **ID**: TLS-202
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:116-176, 194-354
- **Problem**: `compile`, `compileMslPinned`, `compileHlslPinned`, `compileGlslPinned` und `compileMany` wiederholen jeweils: Mutex, `InitializeProcess`, `glslToSpirv`, `try { SPIRV-Cross } catch`, `FinalizeProcess`, `reportShaderResult`. Zusätzlich ist die Stage-zu-`spv::ExecutionModel`-Umrechnung (Zeilen 235-237 und 277-279) kopiert, und die MSL-Optionen (`platform = macOS`, `set_msl_version(2, 3)`) stehen doppelt (Zeilen 132-135 und 219-221). `spirvToSource` ist ein `switch` mit Sonderfall `Glsl430` (Zeile 124-128), der `emitGlsl430(spirv, {}, Stage::Fragment)` mit Dummy-Stage aufruft, und die gepinnten Varianten umgehen `spirvToSource` komplett. Ein neues Target (Pin/Option) braucht also vier Stellen, und genau diese Fehlerklasse hat laut `ShaderCompilerStub.cpp:18-23` schon einmal drei Plattformen unbuildbar gemacht.
- **Code-Auszug**:
```cpp
b.stage = (p.stage == Stage::Vertex)   ? spv::ExecutionModelVertex
        : (p.stage == Stage::Fragment) ? spv::ExecutionModelFragment
                                       : spv::ExecutionModelGLCompute;
```
- **Lösung**:
  1. Helfer `spv::ExecutionModel toExecModel(Stage)` neben `toEsh`.
  2. Helfer `template<class Emit> Result runPipeline(const char* what, const std::string& glsl, Stage, Emit&& emit)`: hält Mutex und (nach TLS-201) Process-Guard, ruft `glslToSpirv`, führt `emit(Result&)` in `try/catch` aus (Prefix für die Log-Zeile als Parameter), loggt via `reportShaderResult`.
  3. `compile` = `runPipeline(..., [&](Result& r){ r.ok = spirvToSource(...); })`; die gepinnten Varianten übergeben nur ihren Emit-Lambda. `compileMany` bleibt eigen (mehrere Targets), nutzt aber denselben Guard.
  4. `makeMslOptions(const MslOptions*)` für die gemeinsamen MSL-Grundoptionen.
- **Erwarteter Effekt**: ca. 60-80 Zeilen weniger, eine Stelle für Init/Locking/Fehlerbehandlung. Keine Laufzeitänderung (Schätzung).
- **Risiko / Verifikation**: Reines Refactoring; die Log-Texte ("SPIRV-Cross(MSL pinned): ...") sollten unverändert bleiben, falls Tests darauf matchen (`grep -rn "SPIRV-Cross(" tests/`). Gleichzeitig `ShaderCompilerStub.cpp` unverändert lassen; die Header-Signaturen ändern sich nicht.

### TLS-203 – Packer-CLI nutzt den Inkrementell-Cache des HpakWriter nicht
- **ID**: TLS-203
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Tools/src/Packer/main.cpp:110-126; Gegenstück src/HE_Core/include/Hpak/HpakWriter.h:90-108
- **Problem**: `HpakWriter::addDirectories` akzeptiert einen `Hpak::IncrementalCache`, mit dem unveränderte Einträge (gleicher Hash des umgeschriebenen Blobs) ohne erneutes Komprimieren/Verschlüsseln übernommen werden. Laut Kommentar in `HpakWriter.cpp:1047` ist genau dieser Durchlauf (Rewrite, Cook, Kompression, Verschlüsselung; dazu BC7/ASTC-Encoding der Texturen) der teure Teil. Der Editor-Export (`ProjectExporter.cpp`) nutzt den Cache, das CLI nicht: `addDirectories(roots, settings)` ohne `progress` und `cache`. Jeder CI-/Build-Skript-Lauf von `hpak_packer` packt also immer von null, auch wenn sich ein Asset geändert hat. Es gibt zudem keine Fortschrittsausgabe, bei großen Projekten läuft das Tool minutenlang stumm.
- **Code-Auszug**:
```cpp
HpakWriter packer;
const int added = packer.addDirectories(roots, settings);
```
- **Lösung**:
  1. Option `--cache <file>`: Cache laden (falls vorhanden), an `addDirectories` übergeben, nach dem `write` aus `packer.entryInfos()` wieder speichern. Die Lade-/Speicherroutinen aus `ProjectExporter.cpp` in eine gemeinsame Funktion in HE_Core ziehen, statt sie im Tool zu duplizieren.
  2. Optional `--progress` mit dem `AddProgressFn`-Callback (stderr, nur bei TTY oder Flag).
- **Erwarteter Effekt**: Bei Wiederholungsläufen ohne oder mit wenigen Änderungen Packzeit von "voll" auf "Hash plus Kopie" (Schätzung; Faktor hängt stark vom Texturanteil ab, messen).
- **Risiko / Verifikation**: Der Header verlangt, dass der Aufrufer nur einen zu den `PackSettings` passenden Cache liefert (Codec, Verschlüsselung, Schlüssel). Der Cache braucht daher einen Settings-Fingerprint; ohne diesen darf kein Cache geladen werden. Test: gleicher Pack mit und ohne Cache muss byte-identisch sein, plus Test mit geänderten `--codec`/`--secret` (Cache muss verworfen werden). Es existiert bereits Testabdeckung für den Cache im Editor-Pfad (`grep -n IncrementalCache tests/*.cpp`, nicht geprüft).

### TLS-204 – widget_gen meldet Verdrahtungsfehler nur auf stderr und endet mit Exit-Code 0
- **ID**: TLS-204
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WidgetGen/main.cpp:701-733, 806, 840, 847-879
- **Problem**: Die `chain`-Lambda in `dialogGraph` liefert `bool wired`, beide Aufrufstellen verwerfen den Rückgabewert. Schlägt `g.connect` fehl, steht nur "could not wire" auf stderr, `dialogFrame` schreibt trotzdem einen Dialog, dessen Buttons nichts tun, und `main` endet mit 0, weil `saveAsset` gelingt. `mat_gen` (Zeile 20-31) hat dagegen ausdrücklich die Philosophie "Failing, not warning" und bricht ab. Das Generator-Ergebnis wird committet, ein stiller Fehler würde in jedem Projekt landen. Die Kommentar-Absicht im Code ("shipping a dialog whose buttons do nothing") wird vom Code selbst nicht eingelöst.
- **Code-Auszug**:
```cpp
chain(okButton,     "Confirmed", 0.0f);
chain(cancelButton, "Cancelled", 200.0f);
return g;
```
- **Lösung**: `dialogGraph` gibt `bool` oder `std::optional<Graph>` zurück (oder nimmt `bool& ok`); `dialogFrame` reicht den Fehler durch, `main` überspringt den Eintrag und zählt ihn als Fehler (wie `mat_gen`: `if (!good) { ...; continue; }`). Skizze: `bool ok = chain(...) & chain(...); if (!ok) failedGraph = true;`.
- **Erwarteter Effekt**: Kein Laufzeiteffekt; verhindert still defekte Engine-Widgets (Korrektheit).
- **Risiko / Verifikation**: Heute verdrahten beide Ketten erfolgreich, das beobachtbare Ergebnis des Normalfalls bleibt byte-identisch. Test: vorhandener Widget-Library-Test (`grep -ln DialogFrame tests/`), nicht verifiziert.

### TLS-205 – hpak_packer löscht die alte Ausgabedatei, bevor er weiß, ob die neue geschrieben werden kann
- **ID**: TLS-205
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Tools/src/Packer/main.cpp:130-137; src/HE_Core/src/Hpak/HpakWriter.cpp:1204-1207
- **Problem**: Das Tool entfernt `outputFile` vor `write()`, und `write()` öffnet ohnehin mit `std::ios::trunc`. Das `exists`+`remove` ist redundant. Schlimmer: Bricht `write` mittendrin ab (Platte voll, Prozess beendet), bleibt weder das alte noch ein gültiges neues Archiv zurück. In einer Build-Pipeline, die dasselbe Ziel wiederverwendet, ist das ein Robustheitsproblem (Teil-Hpak, der erst zur Laufzeit auffällt).
- **Code-Auszug**:
```cpp
if (std::filesystem::exists(outputFile))
    std::filesystem::remove(outputFile);
if (!packer.write(outputFile)) { ... }
```
- **Lösung**: In ein Temp-File im selben Verzeichnis schreiben (`outputFile + ".tmp"`), bei Erfolg `std::filesystem::rename` (atomar auf demselben Dateisystem; unter Windows `std::filesystem::rename` ersetzt vorhandene Dateien ebenfalls), bei Fehler Temp löschen. Das `exists`/`remove` entfällt.
- **Erwarteter Effekt**: Keine Performance, aber Robustheit der Offline-Pipeline; zwei Syscalls weniger.
- **Risiko / Verifikation**: Kleiner Unterschied: Ein vorhandenes, geöffnetes Zielarchiv (gemappt vom laufenden Spiel) ließe sich unter Windows nicht mehr ersetzen. Das gilt heute aber ebenso für `remove`. Test: Packer-Test (falls vorhanden) mit schreibgeschütztem Zielverzeichnis, erwartetes Verhalten: Exit 1, altes Archiv unverändert.

### TLS-206 – mat_gen: `longestChain` ist O(N·L) mit `std::map` und Mehrfach-Push
- **ID**: TLS-206
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Tools/src/MatGen/main.cpp:204-233
- **Problem**: Für jeden Knoten wird die komplette Link-Liste durchsucht (`for (const MatGraphLink& l : g.links) if (l.dstNode != id) continue;`), und ein Knoten, dessen Vorgänger noch nicht in `memo` liegt, wird bei jedem Wiederbesuch erneut gescannt. Ein Knoten kann außerdem mehrfach auf den Stack gelangen (Mehrfach-Fan-out). `std::map<int,int>` mit `count` plus `find` ist unnötig teuer. Bei Zyklen im Graph liefe die Schleife endlos (nur `connect` verhindert das; ob es Zyklen ablehnt, wurde nicht geprüft). Das Water-Material hat grob 1000-2000 Knoten; offline, also unkritisch, aber die Funktion kann einfacher.
- **Code-Auszug**:
```cpp
for (const MatGraphLink& l : g.links)
{
    if (l.dstNode != id) continue;
    auto it = memo.find(l.srcNode);
    if (it == memo.end()) { stack.push_back(l.srcNode); ready = false; }
```
- **Lösung**: Einmal Eingangs-Adjazenz bauen (`std::unordered_map<int, std::vector<int>> in;` oder Vektor nach Knotenindex), `memo` als `std::unordered_map`/Vektor; oder topologisch per Kahn über die Knoten und ein Durchlauf über die Links. Optional einen Zähler als Zyklusschutz (`if (++guard > nodes*links) break`), falls `connect` Zyklen zulässt.
- **Erwarteter Effekt**: Von O(N·L) auf O(N+L); in der Praxis Millisekunden, nur Lesbarkeit und Robustheit relevant (Schätzung).
- **Risiko / Verifikation**: Ergebnis (`best`) muss identisch bleiben: Vergleich alt/neu auf `waterMaterial()` (Zeile 854 gibt "chain N" aus). Wert vorher notieren.

### TLS-207 – mat_gen: Doppelte Codegen- und Persistenzläufe pro Material
- **ID**: TLS-207
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Tools/src/MatGen/main.cpp:842-884
- **Problem**: Pro Material wird `generateFragment(e.mb.g)` einmal in `main` zur Vorprüfung ausgeführt, ein zweites Mal beim Zurücklesen durch `regenerateMaterialFromGraph` im frischen `ContentManager`. Außerdem wird zweimal gespeichert (Pass 1 Graph, Pass 2 gebackene Felder) und zwei `ContentManager`-Instanzen gebaut. Die Zwei-Pass-Struktur ist laut Dateikopf (Zeile 17-23) gewollt, weil der Packer nur die gebackenen Felder ausliefert. Der Mehraufwand ist bei einem Material offline vernachlässigbar. Der Fund ist nur relevant, falls die Liste wächst (`entries` ist als "append only" gebaut).
- **Code-Auszug**:
```cpp
{ ContentManager cm(outDir); written = cm.saveAsset(a); }
if (written) { ContentManager cm(outDir); ... cm.saveAsset(*m); }
```
- **Lösung**: Keine Änderung nötig, solange es bei einem Material bleibt. Bei Wachstum: Pass 1 überspringen und die gebackenen Felder direkt aus `gen` ins `MaterialAsset` schreiben (nur falls die Felder öffentlich und identisch zu `regenerateMaterialFromGraph` zu füllen sind; das ist riskant, weil der Editor-Ladepfad der Maßstab sein soll).
- **Erwarteter Effekt**: Vernachlässigbar (unter einer Sekunde, Schätzung).
- **Risiko / Verifikation**: Bewusst nicht ändern; die Doppelpass-Absicht ist das Sicherheitsnetz für Byte-Gleichheit mit dem Editor. Als "keine Aktion" dokumentiert.

### TLS-208 – weather_sound_gen: Statistik und Peak-Normalisierung vierfach neu geschrieben
- **ID**: TLS-208
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WeatherSoundGen/main.cpp:192-205, 326-334, 382-387, 399-400, 500-503
- **Problem**: RMS und Peak werden an vier Stellen von Hand berechnet: `finish` (Zeile 192-205), `makeStorm` (Peak-Schleife Zeile 331-333), `makeThunder` (`peakOf`-Lambda, Zeile 382-386, und Normalisierung 399-400) und `main` (Zeile 500-502). `makeStorm` dupliziert die "wenn Peak über Grenze, herunterskalieren"-Logik aus `finish` (Zeile 203-204 gegenüber 333). Zusätzlich rechnet `lfo` pro Sample mehrfach `std::sin` (bis zu 3 Aufrufe je Modulationsterm, in `circular` dreimal durchlaufen), und `svfG` ruft `std::tan` pro Sample bei konstanter Frequenz nicht, aber bei `makeWind`/`makeStorm` pro Sample (mit sich ändernder Sweep-Frequenz, legitim). Die Rechenzeit liegt bei wenigen hunderttausend Samples im Millisekundenbereich, daher nur ein Vereinfachungsthema.
- **Code-Auszug**:
```cpp
double peak = 0.0;
for (double v : x) peak = std::max(peak, std::fabs(v));
if (peak > 0.95) for (double& v : x) v *= 0.95 / peak;
```
- **Lösung**: Zwei kleine Helfer: `double peakOf(const std::vector<double>&)` (aus dem Thunder-Lambda herausziehen) und `void limitPeak(std::vector<double>&, double ceiling)`; `finish` und `makeStorm` nutzen sie, `main` nutzt `peakOf` plus eine `rmsOf`-Funktion. Die Gleitkomma-Reihenfolge der Operationen darf nicht geändert werden (siehe Dateikopf Zeile 39-43: Byte-Gleichheit auf derselben Plattform).
- **Erwarteter Effekt**: Ca. 15-20 Zeilen weniger, keine Laufzeitänderung.
- **Risiko / Verifikation**: Summationsreihenfolge für RMS bleibt gleich (vorwärts, `double`), dann sind die erzeugten Bytes identisch. Prüfen durch Regenerieren in ein Temp-Verzeichnis und Byte-Vergleich mit dem vorherigen Lauf auf derselben Plattform; `tests/test_engine_weather_sounds.cpp` prüft die Naht der committeten Dateien.

### TLS-209 – widget_gen: Magic-Numbers für Anker-Presets und Out-Parameter-Konstruktion
- **ID**: TLS-209
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Tools/src/WidgetGen/main.cpp:99, 466, 512, 567, 605, 763, 160-189 und 202-268
- **Problem**: Anker-Presets stehen als nackte Zahlen im Code (`uiSetAnchorPreset(e, 3)`, `14`, `11`, `4`), nur durch Zeilenkommentare erklärt, während `kUIAnchorFill` eine benannte Konstante hat (Zeile 91). `formRowShell` liefert drei IDs über `int&`-Out-Parameter und jede der drei Form-Row-Funktionen deklariert `int label = 0, help = 0, line = 0;` nur zum Durchreichen. Ein kleines Struct (`struct FormRow { Comp c; int label, help, line; };`) wäre klarer. Außerdem steht Zeile 270 ("A heading with a rule under it...") als verwaister Kommentar über dem Divider-Block, gehört aber zu `sectionHeader`.
- **Code-Auszug**:
```cpp
uiSetAnchorPreset(e, 14);            // right edge, stretched down
...
Comp formRowShell(int& labelId, int& helpId, int& lineId)
```
- **Lösung**: Falls `UIWidget` keine benannten Konstanten für die Presets hat, lokal `constexpr int kAnchorTopStrip = 3, kAnchorRightStretch = 14, kAnchorBottomStrip = 11, kAnchorMiddleLeft = 4;` anlegen (die Werte müssen zur Preset-Tabelle in `UIElements.h` passen, vor der Änderung nachsehen). `FormRow`-Struct statt Out-Parameter. Kommentar an `sectionHeader` verschieben.
- **Erwarteter Effekt**: Lesbarkeit, keine Laufzeitwirkung.
- **Risiko / Verifikation**: Ausgabedateien müssen byte-identisch bleiben (Regenerieren und Diff gegen `EditorDeps/EngineContent/Widgets`).

### TLS-210 – compileMany kopiert das SPIR-V pro Ziel
- **ID**: TLS-210
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Tools/ShaderCompiler/ShaderCompiler.cpp:341-348; Header ShaderCompiler.h:179-185, 273-279
- **Problem**: `r.spirv = mr.spirv;` kopiert für jedes angeforderte Target den vollständigen SPIR-V-Vektor, obwohl `MultiResult::spirv` dieselben Worte schon hält. Die Header-Doku von `Result::spirv` ("populated for every target") verlangt es so, aber `MultiResult` dupliziert die Daten damit n-fach. Bei vier Targets und typisch einigen zehn bis hundert KB pro Shader eine unnötige Heap-Kopie. Außerdem werden die Ziele sequenziell erzeugt, obwohl SPIRV-Cross-Compiler unabhängige Objekte sind (aber siehe TLS-201: der Mutex hält ohnehin alles seriell).
- **Code-Auszug**:
```cpp
Result r;
r.spirv = mr.spirv;
r.ok = spirvToSource(mr.spirv, t, r.source, r.log);
```
- **Lösung**: Prüfen, ob irgendein Aufrufer `perTarget[i].spirv` liest (`grep -rn "perTarget" src tests`). Falls nicht, Kopie weglassen und im Header dokumentieren "nur in `MultiResult::spirv`". Sonst unverändert lassen.
- **Erwarteter Effekt**: Kleine Einsparung pro Material-Cook (Schätzung, nur offline/Cook relevant).
- **Risiko / Verifikation**: Öffentliche Verhaltensänderung der API; nur nach Aufrufer-Check. Unter `HE_ENABLE_SHADERC=OFF` liefert der Stub schon kein SPIR-V.

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/Backends/{D3D11,D3D12,Vulkan}/*Renderer.cpp` rufen `he::shaderc` auf; TLS-201 wirkt dort auf die Ladezeit. Die Metal- und GL-Pfade laufen über `MaterialShaderLibrary` (nicht Teil dieses Slices). Ob die Ergebnisse von `compile*` pro Quelltext-Hash gecacht werden, wurde nicht geprüft; falls nicht, wäre ein Cache (GLSL-Hash plus Target plus Pins) in `MaterialShaderLibrary` der größere Hebel als TLS-201.
- `src/HE_Core/src/Hpak/ProjectExporter.cpp` enthält die Cache-Lade-/Speicherlogik für `IncrementalCache` (TLS-203). Diese sollte in eine gemeinsame Funktion in `src/HE_Core/src/Hpak/` wandern, damit das CLI sie mitnutzt.
- `src/HE_Core/src/Hpak/HpakWriter.cpp:1204-1207`: `write()` hat keinen Atomic-Rename; ein `writeAtomic(path)` im Writer selbst würde TLS-205 für Editor-Export und CLI zugleich lösen.
- Packer-Verschlüsselung (`main.cpp:113-118`) leitet den Schlüssel mit Null-Salt ab; gleiche Passphrase ergibt für alle Spiele denselben Schlüssel. Das ist im Dateikopf als bewusst beschrieben und hier nur als Hinweis für ein mögliches Sicherheits-Audit des Moduls HE_Core/Hpak (`KeyDerivation.h`) vermerkt, nicht als Fund gewertet.

## Unsicher / Messung nötig

- TLS-201: tatsächlicher Gewinn von Einmal-Init gegenüber Init pro Aufruf (Zeit pro `compile` mit/ohne Init in Release auf macOS, Windows, Linux messen).
- TLS-203: Anteil der Texturen (BC7/ASTC) an der Packzeit realer Projekte, daraus der erwartete Gewinn durch den Cache.
- TLS-206: ob `MaterialGraph::connect` Zyklen ablehnt (nicht geprüft).
- TLS-210: ob ein Aufrufer `MultiResult::perTarget[i].spirv` liest.
- `compileGlslPinned` und `emitGlsl430`: lineare Pin-Suche pro Ressource ist O(Ressourcen·Pins); bei Pin-Listen unter 40 Einträgen vernachlässigbar, nicht gemessen.
- Ob `mat_gen`/`widget_gen`/`mesh_gen`/`weather_sound_gen` in CI regeneriert werden (dann wäre die Laufzeit relevant); laut Dateikopf nicht Teil des Build-Graphs.

## Gelesen

Alle elf Dateien des Slice 2 vollständig gelesen: ja (`MatGen/main.cpp`, `MeshGen/main.cpp`, `Packer/HpakWriter.h`, `Packer/KeyDerivation.h`, `Packer/main.cpp`, `ShaderCompiler/ShaderCompiler.cpp`, `.h`, `ShaderCompilerStub.cpp`, `spike_main.cpp`, `WeatherSoundGen/main.cpp`, `WidgetGen/main.cpp`). `src/HE_Tools/src/MeshGen/main.cpp` hat keine Funde mit Gewicht: die Kopie `Mesh mesh = meshConst;` (Zeile 369) und fehlendes `reserve` in `Mesh::addVertex` sind offline irrelevant.
