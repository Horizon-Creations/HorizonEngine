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

## Schritt 2: Depth im gespielten Spiel (NN-WS03, RTX 4070, Release, `HE_GPU_DEBUG=1`)

Der Editor zeichnet die Szene immer in sein Viewport-RT. Den Swapchain-Depth
benutzt nur die Spiel-Runtime (`!useViewport`). Geprüft wurde deshalb ein
**exportiertes** Spiel.

**Aufbau**

- **Zeugenszene** `Depthy` (`d3d12-swapchain-resize-witness-scene.ps1`):
  Boden, Rückwand und acht Würfel, die sich über das ganze Bild überlappen.
  Die Überlappung unten rechts liegt bei 1800×1000 außerhalb der 1280×720
  des Starts.
- **Export** über die MCP-Brücke eines privaten Editors mit `project_package`
  (`d3d12-swapchain-resize-export.py`). In der `config.json` des Exports:
  `GameBackend=D3D12`, `GameWindowMode=Windowed`, `PauseOnFocusLoss=false`.
- **Resize-Folge** (`d3d12-swapchain-resize-game-verify.ps1`):
  - Start 1280×720, dann 1800×1000, 960×540 und 1600×900 per `SetWindowPos`
    von außen, ohne Aktivierung.
  - 30 schnelle Zufallsgrößen im Abstand von 30 ms, danach 1800×1000.
  - Langsame Treppe 1700 → 1100 → 1500, alle 400 ms ein Schritt, danach
    1600×900.
  - Minimieren und Wiederherstellen, zum Schluss 1280×720.
  - Nach jedem Schritt eine Aufnahme mit `PrintWindow`.
- **Nicht** über `HE_CAPTURE_FRAME` aufgenommen: Das legt ein Viewport-RT an
  (`Application.cpp:569`), und das Spiel liefe dann auf dem Editor-Pfad.
- **Maß** (`d3d12-swapchain-resize-game-compare.ps1`): Jede Aufnahme wird
  gegen einen **Frischstart** des Spiels in derselben Größe verglichen.
  Gezählt werden die abweichenden Pixel unterhalb der oberen 25 %; das
  Himmelsband darüber trägt die Wolkenanimation.
- **Rauschboden:** Zwei Frischstarts in 1800×1000 weichen dort in 0 von
  1 350 000 Pixeln ab.

**Drei Runtimes, die sich nur in `HorizonRendering.dll` unterscheiden**
(abweichende Pixel unterhalb des Himmels / davon außerhalb 1280×720):

| Schritt                  | Fix (30bade72)        | vorher (main e5c43b86)     | Negativkontrolle (Fix ohne `createSceneDepth` im Resize) |
|--------------------------|-----------------------|----------------------------|----------------------------------------------------------|
| 1800×1000                | 0 / 0                 | 856 252 / 480 082          | 748 400 / 748 400                                        |
| 960×540                  | 0 / 0                 | 196 553 / 0                | 0 / 0                                                    |
| 1600×900                 | 0 / 0                 | 605 331 / 255 519          | 446 400 / 446 400                                        |
| nach schnell, 1800×1000  | 0 / 0                 | 856 252 / 480 082          | 748 400 / 748 400                                        |
| nach langsam, 1600×900   | 0 / 0                 | 605 331 / 255 519          | 446 400 / 446 400                                        |
| wiederhergestellt        | 0 / 0                 | 605 331 / 255 519          | 446 400 / 446 400                                        |
| zurück 1280×720          | 0 / 0                 | 0 / 0                      | 0 / 0                                                    |
| Resize-Zeilen im Log     | 86, Swapchain = Client | 0                         | 86                                                       |
| Debug-Layer / `[ERROR]`  | 0                     | –                          | 780 Warnungen: Renderbereich 1800×1000, Depth 1280×720, „results are undefined“ |

- **Mit Fix** ist jedes Bild nach dem Resize unterhalb des Himmels
  pixelgleich mit dem Frischstart gleicher Größe. Das gilt nach der schnellen
  Folge, nach der langsamen Treppe, nach Minimieren/Wiederherstellen und
  außerhalb der Startausdehnung.
- **Vorher** ist das Bild gestreckt: Es weicht bei jeder Größe außer der
  Startgröße ab.
- **Die Negativkontrolle** zeigt, was der Depth-Teil des Fixes leistet:
  - Beim Vergrößern bleibt alles außerhalb des alten 1280×720-Depth schwarz.
    Die Karte beschneidet dort, und der Debug-Layer warnt bei jedem Draw.
  - Beim Verkleinern (960×540) bleibt das Bild sauber, denn ein größerer
    Depth ist zulässig.
  - Der Spieltest sieht den Depth-Teil also, und der Fix besteht ihn.
- **Editor-Viewport** separat (`d3d12-swapchain-resize-editor-viewport.ps1`,
  D3D12, Würfelszene im Viewport):
  - Jede Größe wurde vor und nach der schnellen bzw. langsamen Folge besucht.
    Der Viewport war dabei jedes Mal pixelgleich.
  - Einzige Abweichung ist der innere Teiler des Content-Browsers. Er kehrt
    nach dem Verkleinern nicht auf seine Breite zurück; das ist ImGui-Layout,
    kein Rendern.
  - 84 Resizes, Swapchain = Client, 0 Debug-Layer-Meldungen.

**Stolperstellen**

- **`<deploy>/Game` bleibt nach einer reinen DLL-Änderung alt.** Die
  Engine-DLLs kommen nur im POST_BUILD von `HorizonGame` dorthin, und
  `Editor/Game` wird nur beim Relink des Editors daraus kopiert. Der Stand
  nach Schritt 1 hätte also den Vorher-Renderer exportiert (MD5 324B…).
  Abhilfe hier: `HorizonGame.exe` und `HorizonEditor.exe` im Baum löschen und
  neu bauen, danach per Hash prüfen.
- **Die ersten ~7 s nach dem Erscheinen des Fensters ist das D3D12-Spiel
  schwarz**, denn Shader und PSOs werden vor dem ersten Frame gebaut. Das
  Skript wartet deshalb auf „OnInit complete“ im Log. Wer früher aufnimmt,
  sieht auch unter D3D11 und Vulkan nur Schwarz.
- **Den Editor aus dem minimierten Zustand wiederherzustellen holt den
  Vordergrund**, auch mit `SW_SHOWNOACTIVATE`. Das Spiel tut das nicht. Im
  Editor-Lauf ist dieser Schritt deshalb weggelassen; Schritt 1 hat ihn für
  den Editor schon abgedeckt.

## Offen

- **Play-in-Editor wurde nicht gefahren.** Es gibt kein MCP-Werkzeug dafür,
  und Maus oder Tastatur gehörten dem Menschen. Play zeichnet in dasselbe
  Viewport-RT wie der oben geprüfte Editor-Viewport.
- **D3D11 hat dasselbe Problem.** Das ist nur aus dem Code gelesen:
  `ResizeBuffers` kommt in `D3D11Renderer.cpp` nicht vor, und `width`/`height`
  werden nur in `Initialize` gesetzt. Es ist hier nicht behoben.
- Ziehen am Fensterrand durch einen Menschen (modale Größenschleife von
  Windows) ist nicht getestet. Getestet sind nur Größenänderungen von außen.
