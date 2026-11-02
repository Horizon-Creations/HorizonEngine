# D3D11/D3D12-Startzeit: Instrumentierung und Befund (Thema 196, Schritt 1)

Stand 13.10.2026. Geschrieben auf dem Mac (MacBook-Air): **keine D3D-Messung möglich**, hier
gibt es weder Windows-SDK noch D3D-Gerät. Die Instrumentierung ist eingebaut, die zwei Kaltstarts
auf NN-WS03 stehen noch aus (Rezept unten). **Nachtrag Schritt 4:** gemessen und behoben auf NN-WS03, siehe
„Messwerte“ und „Behebung“ weiter unten.

## Was jetzt geloggt wird

Alle Zeilen beginnen mit `[startup]`, also reicht ein `Select-String '\[startup\]' HorizonEngine.log`.

| Stelle | Zeilen |
|---|---|
| `src/HE_Core/src/Application/Application.cpp` (`Run`) | `app phase=window+splash`, `app phase=renderer backend=…`, `app phase=OnInit`, `app first frame presented: frame=N ms=… since Run`, `app slow frame=N busyMs=…` (Frames 1..300 mit ≥ 250 ms Arbeitszeit, also ohne den Idle-Wait). Gilt für alle Backends. |
| `D3D_Shared/D3DStartupTiming.h` (neu) | `timedD3DCompile` (gleiche Signatur wie `D3DCompile`): `D3DCompile <name>/<entry> <profile> ms=… bytes=…`, bis zum ersten Frame jeder Aufruf, danach nur ≥ 50 ms. `Phase`/`Laps` für die Phasenzeilen, `StartupClock` für „first frame“. |
| `D3D11Renderer::Initialize` | `device flags=0`, Phasen `D3D11CreateDeviceAndSwapChain`, `createRTV+createDepth`, `createPipeline` (darin `createPostFX`, `createSSAOPipeline`, `createSkyPipeline`, `createDebugLinePipeline`, `createSkinnedPipeline`, `createUIPipeline`, `createMaterialResources`, `createGiPipelines`, `createDeferredPipelines`), `createCube`, `Initialize`; nach dem ersten `Present`: `D3D11 first frame: ms=… since Initialize, compiles=… compileMs=…` |
| `D3D12Renderer::Initialize` | `gpuDebug=0/1 GBV=0/1`, Laps `debugLayerSetup`, `D3D12CreateDevice`, `infoQueue+CreateCommandQueue`, `DXGIFactory+CreateSwapChainForHwnd`, `rtvHeap+allocators+cmdList+fence`, `createGpuTimer+createDepth`, `createPipeline` (darin Sky/DebugLine/SSAO/Skinned/UI/MaterialResources/Gi), `createPostFXPipelines`, `createDeferredPipelines`, `createCube`, `Initialize`; `D3D12 first frame: …` |

Jede Phasenzeile trägt `compiles=` und `compileMs=`: wie viel der Phase FXC war. PSO-/`Create*Shader`-Zeit
ist der Rest (Treiber-Cache-abhängig, FXC nicht).

## Statischer Befund (verifiziert per Code-Lesen, nicht gemessen)

1. **Debug-Layer im Release: nein.** D3D11 erzeugt das Gerät mit Flags `0` (kein `D3D11_CREATE_DEVICE_DEBUG`).
   D3D12 schaltet Debug-Layer + DRED nur bei `_DEBUG` (MSVC-Debug-Konfiguration) oder `HE_GPU_DEBUG` ein,
   GBV zusätzlich nur mit `HE_GPU_GBV`. Die CI-/Release-Builds sind `CMAKE_BUILD_TYPE=Release` (kein `_DEBUG`).
   `HE_GPU_DEBUG` setzen nur `scripts/deferred-witness/run12.ps1` und `scripts/ui-image-repro/*.ps1`.
   **Aber:** Eine Debug-Konfiguration (aus Visual Studio/CLion) schaltet bei D3D12 Debug-Layer + DRED **und** bei
   beiden Backends `D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION` ein. Die Build-Konfiguration des gemeldeten
   langsamen Starts ist offen (Frage an den Menschen über die Queen, Request 74).
2. **Hauptverdacht: FXC beim Start, bei jedem Start.** Alle Shader werden zur Laufzeit aus HLSL-Quelltext mit
   `D3DCompile` übersetzt, synchron im Haupt-Thread. Es gibt keinen Blob-Cache auf Platte, und FXC hat keinen
   OS-/Treiber-Cache. Allein `createPipeline` übersetzt `kSkyFuncHLSL + kSceneHLSL` (~41 KB) fünfmal
   (VSMain, PSMain, VSDepth, VSDepthInstanced, VSMainInstanced), dazu SSAO, Debug-Linien, Skinned, UI, GI
   (alle Stufen), Deferred (D3D11 in `createPipeline`, D3D12 separat) und PostFX (D3D12 eager).
   Der Sky-Pass übersetzt den kompletten GL-Himmel (Wolken, Aurora, Nebel) per shaderc nach HLSL und
   gibt ihn dann mit `D3DCOMPILE_PREFER_FLOW_CONTROL` an FXC. Bei großen Shadern mit Schleifen ist FXC
   bekannt langsam. Metal (handgeschriebenes MSL, Apple-Compiler) und Vulkan (shaderc → SPIR-V) haben
   diesen Schritt nicht. Das passt zum Symptom „nur D3D langsam“.
3. **Erster Frame:** `WarmupMaterials` reiht nur ein, `drainMaterialWarmup` übersetzt die Materialshader im
   ersten `DrawScene` (VS+PS je Material, ebenfalls FXC). Das zeigen `app slow frame` und die D3DCompile-Zeilen.
   Achtung beim Auswerten: Der Editor öffnet das Projekt erst ab Frame 1, die Warmup-Compiles fallen also
   oft *nach* `D3D1x first frame: compiles=…`. Die Summe stimmt nur über alle `D3DCompile`-Zeilen
   (nach dem ersten Frame nur die ≥ 50 ms, Materialshader liegen in der Regel darüber).

## Metal-Vergleichswert (aus vorhandenen Logs, kein neuer Lauf)

`cmake-build-release/src/HE_Editor/HorizonEngine.log` (10.10., Projekt Catania) und `.1.log` (09.10.), Build vom 28.09.:
Renderer-Init 798 ms bzw. 212 ms, OnInit 106 ms bzw. 455 ms, erster Frame 1,45 s bzw. 1,24 s nach Prozessstart.

## Messrezept für NN-WS03 (PowerShell, Release-Build dieses Zweigs)

~~`HE_DUMP_RHI=D3D11` + `HE_EXIT_AFTER_FRAMES`~~ stürzt bei D3D ohne `HE_DUMP_PATH` in Frame 1 ab. Stattdessen
`scripts/d3d-startup/measure.ps1` (wählt das Backend per Scratch-`config.json`, startet versteckt, druckt die
`[startup]`-Zeilen und die FXC-Summe):
```
.\scripts\d3d-startup\measure.ps1 -Exe C:\hwNN\deploy\Editor\HorizonEditor.exe -OutDir C:\hwNN\runs -Rhi 2 -Label d3d11_kalt -Cold
.\scripts\d3d-startup\measure.ps1 ... -Rhi 3 -Label d3d12_warm            # 1 Vulkan, 2 D3D11, 3 D3D12
.\scripts\d3d-startup\measure.ps1 ... -Rhi 2 -Label d3d11_proj -Project C:\hwNN\proj\X\X.heproj
```
`-Cold` löscht den FXC-Bytecode-Cache (echter Roaming-Ordner) und die NVIDIA-DXCache-Datei dieser Exe.
`%LOCALAPPDATA%\D3DSCache` gibt es auf NN-WS03 nicht.

## Messwerte (NN-WS03, RTX 4070, 13.10.2026, Schritt 4)

Release-Build dieses Zweigs (c309cdb8) in einem privaten Baum (`C:/hw196`, `-DDEPLOY_DIR`), Editor versteckt
(`HE_HIDDEN_WINDOW=1 HE_BACKGROUND_FPS=0 HE_EXIT_AFTER_FRAMES=300 HE_COLLAB_OFFLINE=1`), eigenes `APPDATA`.
**Das Rezept oben mit `HE_DUMP_RHI` stürzt ohne `HE_DUMP_PATH` bei D3D in Frame 1 ab** (Overlay nimmt das Backend
aus der Config): Backend stattdessen per Config wählen, `{"RHI": 2}` = D3D11, `3` = D3D12, `1` = Vulkan.
Lauf 1 war für diesen Exe-Pfad der erste überhaupt (Treiber-Cache kalt), Lauf 2 warm. Projekt = Kopie von
HEProject des Menschen (45 Assets). Skript: `scripts/d3d-startup/measure.ps1`, Logs lagen in `C:/hw196/runs/`.

| Lauf | Renderer-Init | davon FXC (Aufrufe) | OnInit | 1. Frame seit `Run` | langsamer Frame danach |
|---|---|---|---|---|---|
| D3D11 leer, kalt | 8290 ms | 7808 ms (41) | 103 ms | 8737 ms | – |
| D3D11 leer, warm | ≈ gleich | 7761 ms (41) | | Wand ≈ 8,7 s | – |
| D3D11 Projekt | 8270 ms | 7776 ms (41) | 49 ms | 8581 ms | f3 1124 ms (matPS 851 ms FXC) |
| D3D12 leer, kalt | 8868 ms | 8108 ms (46) | 98 ms | 9223 ms | – |
| D3D12 leer, warm | ≈ gleich | 8046 ms (46) | | Wand ≈ 9,1 s | – |
| D3D12 Projekt | 8677 ms | 8066 ms (46) | 106 ms | 9046 ms | f2 1157 ms (matPS 846 ms FXC) |
| Vulkan leer, kalt | 898 ms | – | 72 ms | 1366 ms | – |
| Vulkan Projekt | 620 ms | – | 71 ms | 1005 ms | f2 482 ms |

Phasen D3D11 (kalt): CreateDeviceAndSwapChain 103 ms, **createSkyPipeline 5247 ms (FXC 5033)**, createPostFX 1022
(FXC 1020), createDeferredPipelines 945 (795), createGiPipelines 501 (500), Rest < 100 ms je Phase.
D3D12 (kalt): D3D12CreateDevice 157 ms, **createSkyPipeline 5322 (5050)**, createPostFXPipelines 1038 (1012),
createDeferredPipelines 969 (801), createGiPipelines 550 (499), createSkinnedPipeline 379 (375).
Teuerste Einzel-Compiles (D3D11): Sky-PS `main/main ps_5_0` **5032 ms**, PostFX-PS `main/main` 971 ms,
`resolveClusteredPS` 455, `resolvePS` 335, `scene/PSMain` 290, `gi/GiShadowCS` 184, `gi/GiProbeCS` 135.

**Befund (gemessen):** Beide D3D-Backends sind betroffen, gleich stark. ~90 % des Renderer-Starts ist FXC
(`D3DCompile`) im Haupt-Thread, gut 60 % davon ein einziger Shader (der per shaderc/SPIRV-Cross übersetzte Himmel).
Warm ≈ kalt, weil FXC nichts cacht; alles außer FXC zusammen ist nur ~0,5 s (D3D11) bzw. ~0,6-0,8 s (D3D12).
Debug-Layer aus (D3D11 `flags=0`, D3D12 `gpuDebug=0`), Device/Swapchain ~0,1-0,2 s, Ressourcen nicht messbar.
D3D liegt ~8 s über Vulkan (Renderer-Init 0,6-0,9 s, erster Frame 1,0-1,4 s).
Request 74: Der Deploy des Menschen (`HorizonEngineBuild/Editor`) ist Release (`HorizonCore.dll` → `MSVCP140.dll`,
nicht `140D`), sein Log vom 13.10. 17:21 (D3D12) zeigt ~6,3 s zwischen Swapchain und „material resources created“.

## Behebung: FXC-Bytecode-Cache auf Platte (Schritt 4)

`src/HE_Rendering/src/Backends/D3D_Shared/D3DShaderCache.h`, aufgerufen in `timedD3DCompile` (alle 55 Aufrufe
beider Backends, keiner mit Defines/Include-Handler). Schlüssel = FNV-1a (`Hpak::hash64`) über Quelltext, Entry,
Target, beide Flag-Wörter und Größe+Zeitstempel der geladenen `d3dcompiler_47.dll`. Datei = 136-Byte-Header (wiederholt
die Anfrage feldweise, dazu Größe und Hash des Bytecodes) + DXBC; jede Abweichung ist ein Miss → FXC, dann neu
geschrieben (temp + rename). Nur Erfolge werden gespeichert. Ab 2048 Dateien fliegt das älteste Viertel (ein Treffer
setzt die mtime). Ort: `SDL_GetPrefPath(...)/d3dshadercache` wie `glprogcache` — **SDL fragt die Shell, nicht
`%APPDATA%`**: ein Scratch-APPDATA isoliert den Cache nicht, `HE_D3D_SHADER_CACHE_DIR=<dir>` tut es.
`HE_D3D_SHADER_CACHE=0` schaltet ab, `HE_D3D_SHADER_DUMP=<dir>` schreibt jede Quelle als `.hlsl` (für fxc von Hand).
Die Logzeile eines Treffers endet auf ` cached`, `first frame:` zählt `cached=`.

| Lauf (gleicher Aufbau wie oben) | Renderer-Init | FXC | 1. Frame seit `Run` | langsamer Frame |
|---|---|---|---|---|
| D3D11 Cache leer (+ Treiber-Cache leer) | 8372 ms | 7884 ms | 8699 ms | – |
| D3D11 Cache warm (3 Läufe) | 455-471 ms | 7 ms (41/41 cached) | 787-794 ms | – |
| D3D11 Projekt, 1. / 2. Start | 471 / 469 ms | +matPS 851 / cached | 794 / 787 ms | f2 1129 / **278** ms |
| D3D12 Cache leer (+ Treiber-Cache leer) | 9705 ms | 7973 ms | 10071 ms | – |
| D3D12 Cache warm | 561-618 ms | 7 ms (46/46) | 886-952 ms | – |
| D3D12 Projekt, 1. / 2. Start | 563 / 614 ms | +matPS / cached | 885 / 938 ms | f3 1262 / **264** ms |
| Vulkan Projekt (unverändert, kein D3D-Code) | 648 ms | – | 1028 ms | f2 259 ms |

**Ergebnis:** Ab dem zweiten Start liegt D3D11/D3D12 auf Vulkan-Niveau (erster Frame 0,8-0,95 s gegen 1,0 s; ab dem
zweiten Projektstart auch der langsame Frame, 264-278 ms gegen 259 ms). Der allererste Start und der erste Start nach
jeder Shader-Quelländerung bleiben bei ~8,4 / 9,7 s. Ein Treffer kostet ~0,2 ms; `createSkyPipeline` braucht warm noch
~200 ms (`CreatePixelShader` des 215-KB-Sky-Bytecodes, nicht FXC).

**Verifiziert:** (1) Alle 41 Cache-Dateien von D3D11 byte-identisch mit `fxc.exe` aus derselben Quelle (Dump) und
denselben Flags (`/Gfp` für 1024, `/Ges` für 2048): 41/41, 0 Abweichungen. (2) Negativkontrolle im Editor: ein Byte mitten
im Sky-Bytecode gekippt bzw. Datei halbiert → genau dieser Shader neu übersetzt (40/41 cached, 5,2 s), Exit 0, nächster
Start wieder 41/41. (3) `tests/test_d3d_shader_cache.cpp` (Treffer = gleiche Bytes, Flags/Quelle im Schlüssel,
gekipptes Byte, Kürzung, Fehlschlag schreibt nichts): 57 Assertions grün, mit abgeschaltetem Hash-Check 3 rot.
(4) `he_tests` komplett 5157/5157 (Release, Scratch-APPDATA). (5) Bild-A/B D3D11 (SKYTEST): Boden-Bänder exakt 0, der
Himmel weicht ab, weil die Wolken an `m_wallTime = SDL_GetTicks()` hängen (`D3D11Renderer.cpp` ~7940, `HE_SKY_TIME`
pinnt diesen Pass nicht) und der Lauf mit Cache 8 s früher aufnimmt; Cache-gegen-Cache und Ohne-gegen-Ohne rauschen nur.

**Erstes Mal bleibt teuer — kein Flag hilft:** fxc.exe auf der Sky-PS-Quelle (abzüglich ~1 s Prozessstart):
`/Gfp` Standard, `/O1`, `/O3` ≈ 5,0 s; `/O0` ≈ 4,0 s; `/Od` ≈ 1,0 s, aber doppelt so großer Bytecode (unoptimiert,
GPU-Kosten ungemessen); ohne `/Gfp` 6 s. Nicht geändert.

## Wiederholungsmessung, Build, Tests (Schritt 3, 13.10.2026)

Gleicher Aufbau wie in Schritt 4 (NN-WS03, RTX 4070, Release-Baum `C:/hw196` auf HEAD 01f1fffc, Editor versteckt,
300 Frames, Projektkopie HEProject), aber jeder Lauf mit **privatem** Bytecode-Cache (`measure.ps1 -CacheDir`, neu),
damit der Roaming-Ordner des Menschen unberührt bleibt. „Vorher“ = dasselbe Binary mit `-NoCache`
(`HE_D3D_SHADER_CACHE=0`: kein Hash, kein Datei-Zugriff, jeder Shader durch FXC wie vor 11041d41). Spalte
„Schritt 4 vor Fix“ = das echte Vor-Fix-Binary c309cdb8 aus der Tabelle oben. Median aus 3 Läufen, Bereich in Klammern.

| Backend, Fall | Renderer-Init | 1. Frame seit `Run` | Schritt 4 vor Fix (Init / 1. Frame) |
|---|---|---|---|
| D3D11 leer, **ohne Cache** | 8860 ms (8819-9234) | 9173 ms (9145-9555) | 8290 / 8737 ms |
| D3D11 leer, Bytecode-Cache leer (Erststart)* | 9073 ms | 9406 ms | beide Caches leer: 8372 / 8699 ms |
| D3D11 leer, **Cache warm** | **463 ms** (451-483, 5 Läufe) | **793 ms** (787-799) | |
| D3D11 Projekt, ohne Cache | 8658 / 8835 ms | 8964 / 9162 ms, langsamer Frame 1156 / 1134 ms | 8270 / 8581 ms |
| D3D11 Projekt, warm (ab 2. Projektstart) | 453-470 ms | 753-797 ms, langsamer Frame 273-309 ms | |
| D3D12 leer, **ohne Cache** | 9057 ms (8645-10356) | 9422 ms (9000-10732) | 8868 / 9223 ms |
| D3D12 leer, Bytecode-Cache leer (Erststart)* | 9545 ms | 9922 ms | beide Caches leer: 9705 / 10071 ms |
| D3D12 leer, **Cache warm** | **566 ms** (560-596, 5 Läufe) | **886 ms** (881-913) | |
| D3D12 Projekt, ohne Cache | 8654 / 8631 ms | 9012 / 8984 ms, langsamer Frame 1273 / 1123 ms | 8677 / 9046 ms |
| D3D12 Projekt, warm (ab 2. Projektstart) | 563-574 ms | 880-898 ms, langsamer Frame 260-269 ms | |
| Vulkan leer (6 Läufe) | 639 ms (605-836) | 1022 ms (976-1291, 1291 = allererster Lauf) | 898 / 1366 ms (kalt) |
| Vulkan Projekt (6 Läufe) | 606 ms (599-626) | 991 ms (980-1004), langsamer Frame 255-261 ms | 620 / 1005 ms |
| OpenGL leer (6 Läufe) | 134 ms (134-151) | 562 ms (550-622) | – |
| OpenGL Projekt (3 Läufe) | 134 ms | 557 ms, langsamer Frame 259-273 ms | – |

\* Der NVIDIA-DXCache war hier nur teilweise geleert: `-Cold` löscht nur die Dateien, die der erste Lauf eines
OutDir aufgezeichnet hat, und das war ein D3D11-Lauf. Die D3D12-Zeile lief daher vermutlich mit warmem Treiber-Cache.
Die Werte mit beiden Caches leer stammen aus Schritt 4.

**Ergebnis:** Warm sinkt der erste Frame auf D3D11 von 9,2 auf 0,79 s (-91 %), auf D3D12 von 9,4 auf 0,89 s (-91 %).
Damit liegen beide unter Vulkan (1,0 s) und über OpenGL (0,56 s). Ab dem zweiten Projektstart ist auch der Material-Frame
(matPS über FXC, ~850 ms) weg: 260-309 ms, so viel wie Vulkan/GL (255-273 ms). FXC-Summe warm 6-7 ms bei 41/41 bzw. 46/46
Treffern. Die Werte aus Schritt 4 sind damit reproduziert. **Erststart unverändert:** Mit leerem Cache bleibt es bei 9,4 s
(D3D11) bzw. 9,9 s (D3D12). Das gilt für den allerersten Start und für den ersten Start nach jeder HLSL-/Sky-Änderung, also
praktisch nach jedem Engine-Update, das Shader anfasst. Ohne Cache misst dieser Lauf ~0,5 s mehr als Schritt 4 vor dem Fix
(Sky-PS 5,2-6,2 s gegen 5,0 s). Das ist Streuung der FXC-Zeit zwischen Sitzungen, nicht der Cache: abgeschaltet ruft er
nichts auf.
**OpenGL** lief mit warmem `glprogcache` im echten Roaming-Ordner (SDL ignoriert `%APPDATA%` auch dort). Ein
Vorher-Wert für GL fehlt; dass GL nicht langsamer wurde, folgt aus dem Code (nächster Absatz), nicht aus einer Messung.
**Andere Backends:** Der Fix (11041d41) ändert nur `D3D_Shared/*`, den Test und die Doku. Für Vulkan, GL und Metal kommen
von diesem Zweig nur die `[startup]`-Zeilen in `Application.cpp` dazu (drei `steady_clock`-Lesungen, Log-Zeilen bis
Frame 300). Vulkan misst 0,61-0,64 s Init und 1,0 s bis zum ersten Frame, so viel wie Schritt 4 vor dem Fix (0,62 / 1,0 s
mit Projekt). **Metal ist nicht gemessen** (Windows-Gerät). Die Metal-Dateien sind unberührt, und CI 38432202765 auf
11041d41 ist grün (macOS, Windows, Linux, Linux lavapipe).
**Build/Tests:** Alle Targets in `C:/hw196` ohne Restarbeit (`cmake --build -j 8`, ninja: nichts zu tun). Die
Shader-Prüfung meldet 55 HLSL + 58 GLSL fehlerfrei. `he_tests` komplett (Release, Scratch-APPDATA,
`HE_NET_LOOPBACK_ONLY=1`): 5157/5157 Fälle, 1 305 582 Assertions, 0 Fehler, 382 s. `test_d3d_shader_cache` allein: 57/57.
**D3D12-Bytecode gegen fxc.exe** (offen aus Schritt 4): Ein D3D12-Projektlauf mit `HE_D3D_SHADER_DUMP` lieferte 44
eindeutige Schlüssel. Jede Quelle wurde mit `fxc.exe` 10.0.26100 und den Flags aus dem Datei-Header übersetzt: **44/44
byte-identisch**. Negativkontrolle: ein gekipptes Byte im Sky-PS einer Cache-Kopie meldet genau diesen Shader (43/1).
**Auffälligkeiten (nicht Teil dieses Themas):**
(1) D3D12 braucht beim Beenden 3,0 s zwischen `D3D12Renderer: shutdown — waiting for GPU` und `all resources released`
(`D3D12Renderer::Shutdown`, `D3D12Renderer.cpp` ~10013). Das tritt mit und ohne Cache auf, deshalb läuft ein D3D12-Lauf
insgesamt ~9 s statt ~6 s. Ursache nicht untersucht.
(2) D3D12 übersetzt 5 Shader pro Start doppelt (46 Aufrufe, 41 Schlüssel): Der Erststart hat schon 5 Treffer.
(3) Ein Process-Listing direkt nach `HasExited` zeigt die eben beendete PID noch kurz. Es lief kein fremder Editor parallel
(vor jedem Lauf geprüft).

## Handoff

**Erledigt (Schritt 3, NN-WS03):** Messung vorher/nachher auf D3D11, D3D12, Vulkan und GL (Tabelle oben, Logs in
`C:/hw196/runs3`, `runs3b`). Voller Build und `he_tests` grün. D3D12-Bytecode 44/44 gleich fxc. `measure.ps1` hat jetzt
`-CacheDir` und `-NoCache`. Davor Schritt 4: Messung, Behebung per Bytecode-Cache (11041d41) und Test.
**Entscheidungen:** „Vorher“ wird mit demselben Binary und abgeschaltetem Cache gemessen, gegengelesen mit dem echten
Vor-Fix-Binary aus Schritt 4. Für den Vulkan-Vergleich wurde kein eigener release/0.7.0-Build gebaut: Der Fix ändert nur
D3D-Dateien.
**Nicht verifiziert:** Metal-Startzeit (kein Mac, nur CI grün). HorizonGame (gleicher Codepfad, nicht gemessen).
GPU-Laufzeit (Bytecode identisch). Der Deploy des Menschen profitiert erst nach dem Merge und einem Start, der den Cache füllt.
**Fallen:** `HE_DUMP_RHI` ohne `HE_DUMP_PATH` stürzt D3D ab, das Backend kommt per config.json. Der Cache liegt im echten
Roaming-Ordner, für Messungen daher `-CacheDir`. Der Wolkenhimmel in D3D11-Captures hängt an der Zeit seit Start. Die
FXC-Zeit streut zwischen Sitzungen um ~0,5 s. Die DXCache-Liste von `measure.ps1` wird nur beim ersten Lauf
eines OutDir angelegt; für einen echten Kaltstart je Backend einen eigenen OutDir nehmen.
**Nächste Schritte:**
1. Optional, eigenes Thema: den Erststart senken (9-10 s). Compiles parallel per JobSystem (untere Grenze ≈ Sky-PS ~5 s)
   oder Sky asynchron bauen und anfangs den kleinen `kSkyPSHLSL` zeichnen. Beides betrifft `createSkyPipeline` in beiden
   Backends.
2. Optional: die 3 s Shutdown-Wartezeit von D3D12 und die 5 Doppel-Compiles ansehen.
3. Bestätigung durch den Menschen auf seinem Deploy nach dem Merge (zweiter Start).
