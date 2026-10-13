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
