# D3D-Erststart: Shader-Vorkompilierung (Thema 200, Schritt 1, 18.10.2026)

Folge zu Thema 196 (`docs/d3d-startzeit-messung-2026-10-13.md`). Dort kam der FXC-Bytecode-Cache dazu
(`D3D_Shared/D3DShaderCache.h`). Ab dem zweiten Start liegt D3D11/D3D12 damit bei 0,8-0,9 s. Der **erste**
Start bleibt bei 9,4 / 9,9 s, ebenso der erste nach einer Änderung an einem Shader oder an `d3dcompiler_47.dll`
(System32, wandert mit Windows-Updates). Alle ~41 Compiles laufen dann nacheinander im Haupt-Thread. Der
per shaderc übersetzte Himmel (Sky-PS) kostet davon allein ~5 s.

**Achtung, Abhängigkeit:** Thema 196 lag beim Start dieses Schritts nicht auf `release/0.7.0`. Dieser Zweig hat
`origin/claude/d3d11-d3d12-startzeit` hineingemergt (b0872e6f). Der Merge dieses Themas bringt 196 also mit.

## Strategien im Vergleich

| Weg | hilft beim Erststart nach Release | hilft nach eigener Shader-Änderung | Untergrenze / Haken |
|---|---|---|---|
| **A. Parallel-Prefetch** (umgesetzt) | ja | ja | Sky-PS (~5 s ruhig). FXC ist pro Aufruf einthreadig |
| B. Sky asynchron, anfangs `kSkyPSHLSL` | ja | ja | erster Frame ≈ Rest parallel (~1-1,5 s). Himmel springt nach ~5 s um. Dump/Capture/`HE_EXIT_AFTER_FRAMES` müssen synchron bleiben |
| C. Mitgelieferter Laufzeit-Cache | **nein**, so wie er ist | nein | Schlüssel enthält Größe+Zeitstempel der `d3dcompiler_47.dll` aus **System32** (NN-WS03: 10.0.26100.9711). Die DLL wird nicht mitgeliefert (Deploy geprüft), also passt der Schlüssel auf keinem anderen Windows-Stand |
| D. Seed beim Build (Tool, Schlüssel ohne Compiler-Identität, read-only neben der Exe) | ja (~warm) | ja, wenn der Build ihn neu erzeugt | braucht ein C++-Tool mit shaderc (das Python-`validate_embedded_shaders.py` sieht den Sky nicht). Requests müssten an einer Stelle stehen, die Backends und Tool teilen |
| E. andere FXC-Flags | – | – | in 196 gemessen: nur `/Od` ist schnell (1 s), gibt aber doppelten Bytecode. Verworfen |

**Entscheidung:** A als Prototyp, weil es beide Fälle abdeckt und nichts am Bild ändert. B und D sind die
nächsten Hebel, denn A kann den Sky nicht unterbieten (Messung unten). C nur in der Form D.

## Prototyp A (umgesetzt)

* `src/HE_Rendering/src/Backends/D3D_Shared/D3DShaderPrefetch.h`: `submit()` (:121) legt eine Anfrage als Job auf
  `globalPool()`. Der Job macht nur Cache-Lesen bzw. `D3DCompile` + Cache-Schreiben (`run`, :95). Die Quellen baut
  das Backend selbst im eigenen Thread (shaderc/MaterialShaderLibrary). `take()` (:158) wartet auf den Job. Ein
  noch wartender Job läuft dabei inline (`JobHandle::wait`). Danach gibt `take()` hr/Code/Fehler-Blob zurück.
  Zugeordnet wird über den Cache-Schlüssel, und der Slot wiederholt die Anfrage Feld für Feld. Eine Anfrage, die
  von ihrer Aufrufstelle abweicht, wird daher nie ausgegeben, sie kostet nur Worker-Zeit. `finishStartup()`
  (:204) schreibt beim ersten Frame `[startup] D3D1x shader prefetch: submitted= joined= cached= workerMs= unused=`
  und nennt nicht genutzte Anfragen (Drift-Melder). `drain()` (:233) verwirft im Shutdown, was noch wartet, und
  wartet auf laufende Jobs. `DrainOnFailure` ruft `drain()` auf, wenn `Initialize` wirft: `Application::Run`
  meldet den Fehler und kehrt ohne `Shutdown` zurück, ein laufender Sky-Job würde sonst die Renderer-DLL
  überleben. `HE_D3D_SHADER_PREFETCH=0` schaltet den Prefetch ab (serieller Pfad, gleiche Binary).
* `D3DStartupTiming.h` `timedD3DCompile` (:73): fragt zuerst `take()` (:92). Bei einem Join ist `ms=` die Wartezeit
  des Haupt-Threads, und die Logzeile trägt `prefetched worker=<FXC-Zeit>`. `first frame:` zählt `prefetched=`.
* D3D11 `D3D11Renderer.cpp`: `skyCrossHlsl()` (:5468) übersetzt den Sky einmal pro Prozess, Prefetch und
  `createSkyPipeline` teilen ihn. `prefetchSkyShaders()` (:5502) läuft ganz oben in `Initialize` (:6234), vor dem
  Device. `prefetchPipelineShaders()` (:5515) schickt alle übrigen 39 Anfragen von `createPipeline` vor deren
  Aufruf ab (:6282). `drain` steht im Shutdown (:6310). D3D12 genauso: :2803 / :2836 / :2850, Aufrufe :10060 /
  :10222, Shutdown :10257.
* `scripts/d3d-startup/measure.ps1 -NoPrefetch`. Die Summenzeile zählt `prefetched=`.

## Messung (NN-WS03, Release `C:/hw200`, privater Deploy, je Lauf eigener `-CacheDir`)

**Vorbehalt:** Während der Messung lief auf dem Gerät ein Spiel des Menschen (CPU 35-57 %). Der Sky-PS brauchte
deshalb 13-18 s statt der 5 s aus Thema 196. Absolute Werte sind nicht mit 196 vergleichbar. Mit und ohne
Prefetch liefen abwechselnd, mit derselben Binary.

| Lauf | erster Frame seit `Run` | Sky-Phase | FXC Haupt-Thread |
|---|---|---|---|
| D3D11 kalt, Prefetch (2) | **13,6 / 19,2 s** | 10,7 / 16,4 s | 41/41 joined, unused 0 |
| D3D11 kalt, `-NoPrefetch` (2) | 20,9 / 21,6 s | 15,9 / 15,9 s | 19,8 / 20,4 s |
| D3D11 warm, Prefetch / ohne (je 2) | 1,18 / 1,12 s gegen 1,11 / 1,08 s | – | 41/41 cached |
| D3D12 kalt, Prefetch / ohne | **22,0 s** gegen 26,7 s | 18,9 / 19,8 s | 46 Aufrufe, 41 Schlüssel, alle joined |
| D3D12 warm, Prefetch / ohne | 1,44 s gegen 1,44 s | 0,2 s | – |

**Ergebnis:** Der Prefetch nimmt beim Erststart alles weg, was nicht Sky ist, unter Last 2,4-7,3 s (D3D11) bzw.
4,7 s (D3D12). Übrig bleibt die Sky-Phase, und sie ist jetzt die Untergrenze. Auf ruhigem Gerät (Sky 5 s, Rest
~2,9 s) folgen daraus rechnerisch ~5,5 statt 9,4 s. **Das ist noch nicht gemessen** (Schritt 2). Warm ändert sich
nichts. `prefetchPipelineShaders` kostet 0,2-0,27 s auch mit `-NoPrefetch`: die shaderc-Übersetzungen der
MaterialShaderLibrary sind nur vorgezogen, nicht zusätzlich. Die gleichen Warm-Werte bestätigen das.
**FXC parallel:** Ein eigener Bench (`C:/hw200/fxcbench`, Sky-Quelle aus 196) unter derselben Last ergab
Haupt-Thread 15-19 s, `std::thread` 19 s, 2 gleichzeitig je 20 s, 4 gleichzeitig je 29 s. Ein Thread ist also
nicht von sich aus langsamer. Mehrere große FXC-Läufe bremsen sich aber gegenseitig.

## Verifiziert / nicht verifiziert

* **Bytecode gleich:** Die Cache-Verzeichnisse aus Prefetch- und seriellen Läufen sind byte-identisch, D3D11 41/41
  (zwei Paare) und D3D12 41/41.
* `tests/test_d3d_shader_cache.cpp`: drei Fälle, 106 Assertions grün. Neu: ein Join liefert die Bytes von direktem
  `D3DCompile`, eine doppelte Anfrage wird nur einmal abgeschickt, ein Fehler kommt mit Fehler-Blob durch und
  schreibt nichts, eine abgewichene Anfrage wird nicht beantwortet. Außerdem 8 Threads FXC (zwei Quellen gemischt)
  = seriell, mit genau einer Datei pro Schlüssel und ohne `.tmp`-Reste. Keine Negativkontrolle gebaut. Diskriminierend
  sind der `prefetched`-Zähler und der Byte-Vergleich.
* `he_tests` komplett (Release, Scratch-APPDATA): 5520/5522. Die 2 Fehler stecken in `test_profiler.cpp` (:107, :215,
  `EngineProfiler`-Singleton, abhängig von der Reihenfolge). Mit `--source-file-exclude=*test_d3d_shader_cache.cpp`
  kommen dieselben 2 Fehler (5517/5519), allein ist die Datei grün (18/18). Sie stammen also nicht von diesem Zweig.
  Gegen `release/0.7.0` nicht gegengeprüft. `validate_embedded_shaders.py`: 55 HLSL + 58 GLSL fehlerfrei.
  Warm-Smoke nach dem Guard: D3D11 und D3D12 Exit 0, alles joined.
* Nicht verifiziert: ruhiges Gerät, Projekt-Start (Materialshader laufen weiter seriell im ersten Frame, nicht
  vorab bekannt), HorizonGame, Metal/Linux (nur `D3D_Shared`/D3D-Dateien geändert, dort nicht kompiliert), Debug-Build.

## Schritt 2: Messung D3D11/D3D12, Shader-Änderung, Tests (18.10.2026)

Gleiche Binary wie Schritt 1 (d800912a, Release `C:/hw200`, privater Deploy). Neu: `scripts/d3d-startup/measure_ab.ps1`.
Eine Runde = kalt mit Prefetch, kalt `-NoPrefetch`, danach je ein Warmstart auf den beiden frisch gefüllten
Cache-Verzeichnissen. Die Reihenfolge pf/nopf wechselt jede Runde. Vor jedem Lauf misst das Skript die CPU-Last
(`LoadPercentage` und den fremden Prozess mit der meisten CPU in 2 s). Alle Zeilen stehen in `C:/hw200/runs1{1,2}/ab.csv`
(72+24 Läufe, alle exit=0). Wert = `app first frame presented` seit `Run`. Mit Projekt dazu, wann der langsame
Projekt-Frame (Materialshader) fertig war.

**Ruhiges Gerät** (Last 0-11 %; die letzten D3D12-Projektläufe 12-31 %, Minecraft lief an), je 3 Läufe:

| Lauf | Prefetch | `-NoPrefetch` | Gewinn |
|---|---|---|---|
| D3D11 kalt | 6,62 / 6,19 / 6,29 s | 9,52 / 9,08 / 9,15 s | **-2,9 s (-31 %)** |
| D3D12 kalt | 8,30 / 8,68 / 8,07 s | 11,84 / 11,30 / 10,29 s | **-2,8 s (-25 %)** |
| D3D11 kalt, Projekt (fertig) | 6,61 / 6,57 / 6,62 (7,77 / 7,75 / 7,80) s | 9,15 / 9,40 / 9,37 (10,33 / 10,57 / 10,56) s | -2,7 s |
| D3D12 kalt, Projekt (fertig) | 7,89 / 9,00 / 8,32 (9,23 / 10,34 / 9,75) s | 10,94 / 10,69 / 11,32 (12,24 / 12,07 / 12,77) s | -2,5 s |
| D3D11 warm | 1,23* / 0,80 / 0,79 s | 0,78 / 0,78 / 0,81 s | gleich |
| D3D12 warm | 0,90 / 0,91 / 0,89 s | 0,90 / 0,94 / 0,96 s | gleich |
| D3D11 / D3D12 warm, Projekt | 0,79-0,80 / 0,97-1,07 s | 0,80-0,81 / 0,98-1,07 s | gleich |

\* erster Lauf direkt nach dem Ende des Spiels, Ausreißer.

**Unter Last** (Spiel des Menschen, 3-12 Kerne, Last 17-60 %), je 3 Läufe: D3D11 kalt 12,6 s gegen 15,6 s, mit Projekt
12,0 gegen 14,4 s. D3D12 kalt 11,2 gegen 16,0 s, mit Projekt 12,3 gegen 15,7 s. Warm überall gleich (±0,1 s).

**Untergrenze ist der Sky.** Ruhig wartet D3D11 in `createSkyPipeline` 4,4-4,9 s auf den Sky-PS-Job. Auf D3D12 braucht
der Job `worker=6,3 s` (seriell 5,3-5,9 s, die parallelen Jobs bremsen ihn), danach ~1,3 s PSO-Erzeugung. Die
Projektion aus Schritt 1 (~5,5 s) war zu optimistisch: gemessen 6,4 s (D3D11) und 8,4 s (D3D12).
Die Materialshader des Projekts (`matPS`, 1,2 s FXC) laufen weiter seriell im Frame 2-7 (`compiles=42/47`, `prefetched=41/46`).

**Bytecode gleich:** alle 18 Paare (Prefetch- gegen `-NoPrefetch`-Cache, je Backend bare/Projekt/ruhig, 3 Runden)
sind byte-identisch, 41/41 bzw. mit Projekt 44/44.

**Shader-Änderung (Regression):** Probe nur lokal, nicht committet. Ein Kommentar in `kDebugLineHLSL`
(`D3D_Shared/HlslSources.h`), neu gebaut (nur die zwei D3D-Renderer), Warmstart auf einer Kopie des warmen Caches.
Auf beiden Backends, mit und ohne Prefetch, wurden genau `dbgline/VSLine` und `dbgline/PSLine` neu übersetzt (mit
Prefetch `prefetched worker=3-4 ms`). Alles andere war `cached`, `unused=0`, der Cache wuchs von 41 auf 43 Dateien
(die alten Einträge bleiben als Waisen liegen, es gibt kein Aufräumen). Dann ein Syntaxfehler in `PSLine`: beide
Wege melden `dbgline/VSLine ... FAILED` und `DebugLine VS compile failed`, schreiben nichts in den Cache, der Editor
läuft weiter (exit 0). Einziger Unterschied: Mit Prefetch meldet die Prefetch-Zeile `unused=1 dbgline/PSLine`, weil
die Pipeline nach dem VS-Fehler abbricht. Das ist harmlos, `unused` heißt dann nicht Drift. Nach dem Zurücksetzen
wieder 41/41 `cached`.

**Tests:** `he_tests` komplett (Release, Scratch-APPDATA): 5519/5522. `test_profiler.cpp:107/:215` wie in Schritt 1.
Nachgestellt mit nur drei Dateien: `--source-file=*test_dev_stats_tile.cpp,*test_profiler.cpp,*test_profiler_stats.cpp`
→ dieselben 2 Fehler. `test_dev_stats_tile` lässt den `EngineProfiler`-Singleton aufnehmen. Keine der Dateien und
nichts von `EngineProfiler` ist auf diesem Zweig geändert. Einen `release/0.7.0`-Build habe ich nicht gegengeprüft.
Der dritte Fehler (`test_contentmanager.cpp:1928`) kam von meinem APPDATA mit `/`. Mit `C:\...` läuft die Datei grün (91/91).

**Neue Falle:** Die NVIDIA-DXCache-Dateien (`<hash>a93b874df862.nvph`) hängen am Exe-**Namen**, nicht am Pfad. Der
eigene Editor des Menschen (`HorizonEngineBuild\Editor\HorizonEditor.exe`) teilt sie mit dem privaten Deploy und hält
eine davon offen. `measure.ps1 -Cold` hat bisher also auch seinen Treiber-Cache gelöscht. Jetzt überspringt es gesperrte Dateien
und meldet `LOCKED`. Die D3D12-Kaltläufe unter Last waren deshalb nur teilweise treiber-kalt (beide Seiten gleich).

## Handoff

**Erledigt:** Schritt 1: Strategievergleich und Prototyp A (paralleler FXC-Prefetch, b39d74a6, 191eda02) in beiden D3D-Backends.
Schritt 2: Messung ruhig und unter Last, kalt/warm, mit und ohne Projekt, je 3 Läufe, abwechselnd mit derselben
Binary (Tabelle oben). Byte-Gleichheit 18/18, Shader-Änderungs- und Fehlerprobe auf beiden Backends, voller Testlauf.
Neu: `scripts/d3d-startup/measure_ab.ps1`. `measure.ps1 -Cold` übersteht gesperrte DXCache-Dateien.
**Ergebnis:** Erststart ruhig D3D11 9,2 → 6,4 s, D3D12 11,1 → 8,4 s. Warm unverändert (0,8 / 0,9 s). Keine
Regression bei Shader-Änderung: Nur der geänderte Shader wird neu übersetzt, ein Fehler verhält sich wie seriell.
**CI:** 39131821097 (CI, alle Plattformen) und 39131821283 (Runtime flavours) auf 191eda02 grün. Danach nur Doku/Skripte.
**Nicht verifiziert:** HorizonGame (exportiertes Spiel), Debug-Build, Profiler-Fehler gegen einen `release/0.7.0`-Build.
**Entscheidungen:** Siehe Schritt 1. Kein Code geändert in Schritt 2, nur Skripte und Doku. Die Probe in `HlslSources.h`
wurde zurückgesetzt (`git status` sauber, Deploy wieder auf HEAD).
**Fallen:** Gerätelast prüfen, und `measure_ab.ps1` schreibt sie in jede Zeile. Ein OutDir ohne `dxcache_mine.txt` macht
`-Cold` treiber-warm. DXCache-Namen teilt man mit jedem `HorizonEditor.exe`: Ist der Editor des Menschen zu, löscht
`-Cold` weiter auch seinen Treiber-Cache (die ruhigen Läufe haben das getan). Vorschlag, nicht geprüft: Den privaten
Deploy unter anderem Exe-Namen starten. `unused=1` nach einem Compile-Fehler ist
kein Drift. Der Bytecode-Cache räumt alte Einträge nie weg und wächst mit jeder Shader-Änderung.
**Nächste Schritte:**
1. Merge mit Thema 196 zusammen (b0872e6f bringt 196 mit).
2. B (Sky asynchron, anfangs `kSkyPSHLSL`): bringt den Erststart auf ~1-1,5 s + PSOs. Vorher die FXC-Zeit von
   `kSkyPSHLSL` messen. Eigenes Thema.
3. D (Seed-Cache beim Build), danach optional Aufräumen verwaister Cache-Einträge (z. B. nach mtime).
4. Profiler-Reihenfolge: `test_dev_stats_tile.cpp` muss das Aufnehmen am Ende abschalten (eigenes kleines Thema).
