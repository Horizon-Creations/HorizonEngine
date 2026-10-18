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

## Handoff

**Erledigt:** Strategievergleich (oben). Prototyp A in beiden D3D-Backends mit Test, `measure.ps1 -NoPrefetch`,
A/B-Messung unter Last und Bytegleichheit. Zweig `claude/d3d-erststart-shader-vorkompilierung` mit 196 hineingemergt.
**Entscheidungen:** Prefetch über eine handgeschriebene Liste je Backend. Nur so geht es beim allerersten Start.
Die Drift fängt der `unused=`-Zähler, falsche Shader sind ausgeschlossen. Das Clustered-Resolve wird auf Verdacht
abgeschickt. Mitgelieferter Cache in heutiger Form verworfen (System32-Compiler im Schlüssel). Async-Sky nur
beschrieben, nicht gebaut.
**Fallen:** Das Gerät war während der Messung durch ein Spiel belastet. Vor Schritt 2 `Get-Process` und die
CPU-Last prüfen, sonst schwankt der Sky um ±3 s. `kSkyVSCrossHLSL` liegt in `HE::glsl`. Eine neue eager
`timedD3DCompile`-Stelle in `Initialize` gehört in `prefetchPipelineShaders`, sonst läuft sie seriell. Sichtbar
wird das nur als Abstand `compiles=` minus `prefetched=` in der `first frame:`-Zeile (ohne Projekt 0). `unused`
meldet nur die Gegenrichtung.
**Nächste Schritte:**
1. Schritt 2: auf ruhigem Gerät messen (D3D11/D3D12, kalt/warm, je 3 Läufe, `-NoPrefetch` als Vorher), dazu mit
   Projekt (`-Project`). Regression bei Shader-Änderung: eine Quelle ändern, dann darf nur dieser Shader neu
   übersetzt werden.
2. Eigener Schritt/eigenes Thema: B (Sky asynchron) senkt den Erststart auf ~1-1,5 s. Vorher die FXC-Zeit von
   `kSkyPSHLSL` messen.
3. Danach D: Seed-Tool beim Build (Requests in eine gemeinsame Quelle ziehen, Schlüssel ohne Compiler-Identität,
   read-only Zweitverzeichnis in `cacheDir`-Nähe).
