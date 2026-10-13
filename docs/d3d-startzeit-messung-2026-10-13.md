# D3D11/D3D12-Startzeit: Instrumentierung und Befund (Thema 196, Schritt 1)

Stand 13.10.2026. Geschrieben auf dem Mac (MacBook-Air): **keine D3D-Messung möglich**, hier
gibt es weder Windows-SDK noch D3D-Gerät. Die Instrumentierung ist eingebaut, die zwei Kaltstarts
auf NN-WS03 stehen noch aus (Rezept unten).

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

```
$env:HE_DUMP_RHI = "D3D11"          # danach "D3D12"
$env:HE_HIDDEN_WINDOW = "0"; $env:HE_EXIT_AFTER_FRAMES = "300"
.\HorizonEditor.exe ; Select-String '\[startup\]' .\HorizonEngine.log
```
Je Backend Lauf 1 kalt (vorher `%LOCALAPPDATA%\D3DSCache` und den Shader-Cache des GPU-Herstellers leeren,
z. B. `%LOCALAPPDATA%\NVIDIA\DXCache`) und Lauf 2 warm. Einmal mit dem echten Projekt des Menschen. FXC ist
in beiden Läufen gleich teuer, nur `Create*Shader`/PSO profitieren vom Treiber-Cache.

## Messwerte (NN-WS03, RTX 4070, 13.10.2026, Schritt 4)

Release-Build dieses Zweigs (c309cdb8) in einem privaten Baum (`C:/hw196`, `-DDEPLOY_DIR`), Editor versteckt
(`HE_HIDDEN_WINDOW=1 HE_BACKGROUND_FPS=0 HE_EXIT_AFTER_FRAMES=300 HE_COLLAB_OFFLINE=1`), eigenes `APPDATA`.
**Das Rezept oben mit `HE_DUMP_RHI` stürzt ohne `HE_DUMP_PATH` bei D3D in Frame 1 ab** (Overlay nimmt das Backend
aus der Config): Backend stattdessen per Config wählen, `{"RHI": 2}` = D3D11, `3` = D3D12, `1` = Vulkan.
Lauf 1 war für diesen Exe-Pfad der erste überhaupt (Treiber-Cache kalt), Lauf 2 warm. Projekt = Kopie von
HEProject des Menschen (45 Assets). Skript: `C:/hw196/measure.ps1` (nicht eingecheckt), Logs `C:/hw196/runs/`.

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

## Handoff

**Erledigt:** Instrumentierung (oben), Debug-Layer-Prüfung (statisch), Metal-Vergleichswert aus Logs.
Verifiziert: `Application.cpp` per `-fsyntax-only` mit den echten Flags des Release-Baums (rc 0);
`D3DStartupTiming.h` per `-fsyntax-only -Wall -Wextra -Wformat` gegen ein D3D-Stub-Header, mit Negativkontrolle.
**Nicht verifiziert:** Die Renderer-.cpp kompilieren nur auf Windows (CI). Es gibt keine einzige D3D-Millisekunde.
Ohne die steht die Aussage „welche Phase kostet“ nur als Verdacht (Punkt 2) da.

**Nächste Schritte, in Reihenfolge:**
1. Auf NN-WS03 diesen Zweig Release bauen, Rezept oben für D3D11 und D3D12 (kalt + warm), `[startup]`-Zeilen in
   diese Datei unter „Messwerte“ eintragen.
2. Antwort auf Request 74 (Debug- oder Release-Build beim Menschen?) einarbeiten. Bei Debug ist Punkt 1 im Befund die Ursache.
3. Erst dann Schritt 2 (Beheben). Naheliegend, wenn FXC dominiert: Bytecode-Cache auf Platte (Schlüssel = Hash aus
   Quelltext + Entry + Profil + Flags), Kompilieren beim Build (fxc offline) oder parallel per JobSystem.
