# D3D12: Haupt-Swapchain folgt der Fenstergröße (Thema 112)

## Ursache

`D3D12Renderer` hat die Swapchain, ihre drei RTVs und den Szenen-Depth des
Swapchain-Pfads (`depthBuffer`, DSV in `dsvHeap[0]`, Decal-SRV
`k_decalSceneDepthSlot`) nur in `Initialize` mit der Startgröße angelegt.
`ResizeBuffers` wurde nirgends aufgerufen, und `width`/`height` blieben
unverändert. Nach einer Fenstergrößenänderung hat DXGI das Bild in Startgröße
über den neuen Client-Bereich gestreckt. ImGui dagegen hat sich für die neue
`DisplaySize` aufgebaut und wurde dabei beschnitten.

Vulkan erholt sich über `VK_ERROR_OUT_OF_DATE_KHR`. D3D hat kein solches Signal,
deshalb muss der Renderer selbst nachsehen.

## Fix

`D3D12RendererImpl::resizeSwapchainIfNeeded()`, aufgerufen am Anfang von
`Render()`, bevor der Frame-Slot abgewartet oder zurückgesetzt wird:

1. `GetClientRect(hwnd)`; bei 0×0 (minimiert) oder unveränderter Größe: nichts tun.
2. `waitForAllFrames()`, alle `renderTargets[i].Reset()`.
3. `ResizeBuffers(k_frameCount, w, h, UNKNOWN, swapchainFlags)`. Die Flags sind
   die Erstellungsflags (`ALLOW_TEARING` auf fähigen Rechnern), weshalb sie jetzt
   in `swapchainFlags` gespeichert werden.
4. `GetBuffer` + `CreateRenderTargetView` in dieselben `rtvHeap`-Slots,
   `frameIndex = GetCurrentBackBufferIndex()`.
5. `createSceneDepth(w, h)`: der aus `createDepth()` herausgelöste
   größenabhängige Teil. DSV-Heap und Schattenarrays bleiben unangetastet.

Schlägt `ResizeBuffers` fehl, werden die alten Buffer neu geholt, und dieselbe
Größe wird nicht jeden Frame neu versucht. Jeder Resize schreibt eine Zeile
`D3D12Renderer: swapchain resized to WxH (client WxH)` ins Log.

## Verifikation (NN-WS03, RTX 4070, Release, `HE_GPU_DEBUG=1`)

Skript: `docs/d3d12-swapchain-resize-verify.ps1`. Es startet den interaktiven
Editor unter D3D12 mit einem eigenen APPDATA. Die Größe ändert es von außen per
`SetWindowPos`, ohne Aktivierung, in dieser Folge: 1280×720, 1800×1000,
960×600, 25 Zufallsgrößen im 40-ms-Takt, 1600×900, Minimieren und
Wiederherstellen, 1280×720. Nach jedem Schritt wird der Client-Bereich per
`PrintWindow` aufgenommen und in Bildmitte gemessen, auf welcher y-Position die
ersten UI-Kanten liegen.

| Schritt   | vorher (main e5c43b86) | mit Fix     |
|-----------|------------------------|-------------|
| 1600×900  | 1,23,24,46             | 1,23,24,46  |
| 1280×720  | 1,18,38,68             | 1,23,24,46  |
| 1800×1000 | 1,27,51,52             | 1,23,24,46  |
| 960×600   | 1,15,16,63             | 1,23,24,46  |

- **Vorher** skalieren die Kanten mit H/900, also mit dem Startbild gestreckt.
  Bei 960×600 füllt die verkleinerte UI nur ein Stück oben links (etwa
  576×400 px, abgelesen), der Rest ist schwarz.
- **Mit Fix** stehen die Kanten bei jeder Größe still.
- Im Fix-Lauf: 60 Resize-Zeilen, und in jeder ist die Swapchain-Größe gleich
  der Client-Größe (0 Abweichungen). Debug-Layer-Meldungen außer der
  bekannten ClearRenderTargetView-Warnung zum Clear-Wert: 0. `[ERROR]`-Zeilen: 0.
- Größenquelle beim Start: Swapchain 1600×900 aus `window->GetWidth/Height`,
  Client-Rect 1600×900. Beide stimmen also überein. DPI-Skalierung ungleich
  100 % ist nicht getestet.
- he_tests: 4102/4102. Der neue Test „D3D12 main swapchain follows the window
  size“ pinnt die Verdrahtung im Quelltext. Gegen die alte Quelle schlägt er
  fehl, das ist geprüft.

## Offen

- **Der Depth-Resize im Swapchain-Szenenpfad (`!useViewport`, Spiel-Runtime)
  ist zur Laufzeit nicht belegt.** Der Editor zeichnet die Szene immer in das
  Viewport-RT. Eine Negativkontrolle, ein Bau ohne `createSceneDepth` im
  Resize, lief im Editor-Skript genauso sauber durch. Der Editor-Test kann den
  Depth-Teil also nicht erkennen. Um ihn zu belegen, bräuchte es ein
  exportiertes Spiel (`HorizonGame.exe`) unter D3D12, dessen Fenster in der
  Größe geändert wird.
- **D3D11 hat dasselbe Problem.** Das ist nur aus dem Code gelesen:
  `ResizeBuffers` kommt in `D3D11Renderer.cpp` nicht vor, und `width`/`height`
  werden nur in `Initialize` gesetzt. Es ist hier nicht behoben.
- Ziehen am Fensterrand durch einen Menschen (modale Größenschleife von
  Windows) ist nicht getestet. Getestet sind nur Größenänderungen von außen.
