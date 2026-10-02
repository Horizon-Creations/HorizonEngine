# D3D11: Bild folgt der Fenstergröße (Thema 128, Schritt 1)

Zwei getrennte Fehler, beide nur beim Ändern der Fenstergröße unter D3D11.

## Ursache 1: Swapchain blieb auf der Startgröße

Thema 112 hat das für D3D12 behoben und für D3D11 nur aus dem Code gemeldet.
Das trifft zu:

- `D3D11Renderer` hat Swapchain, Backbuffer-RTV (`rtv`) und den Depth des
  Swapchain-Pfads (`depthTex`/`dsv`/`depthSRV`) nur in `Initialize` angelegt.
- `ResizeBuffers` kam nirgends vor.
- `width`/`height`, und damit Viewport und `DrawScene`-Größe des Spielpfads,
  blieben auf der Startgröße.

DXGI streckte deshalb das Startbild über den neuen Client-Bereich. ImGui
dagegen baute sich für die neue `DisplaySize` auf. Beim Verkleinern füllte die
UI also nur noch eine Ecke, der Rest war schwarz: Bei 1280×720 aus 2000×1125
waren es etwa 820×460 px oben links (`pre2/01_1280x720.png`).

**Fix:** `D3D11RendererImpl::resizeSwapchainIfNeeded()`, aufgerufen ganz oben in
`Render()`, nach dem Vorbild von D3D12:

1. `GetClientRect(hwnd)`. Bei 0×0 (minimiert), unveränderter Größe oder einer
   zuvor abgelehnten Größe passiert nichts.
2. `OMSetRenderTargets(0, nullptr, nullptr)`, `rtv.Reset()`, `Flush()`.
   Der letzte Frame lässt das RTV am Immediate Context gebunden, und D3D11
   zerstört freigegebene Views verzögert. Ohne diese drei Schritte liefert
   `ResizeBuffers` `DXGI_ERROR_INVALID_CALL`.
3. `ResizeBuffers(0, w, h, UNKNOWN, 0)`. Die Flags sind 0, so wie bei der
   Erstellung (`scd.Flags` wird nie gesetzt).
4. `createRTV()`, und zwar immer, auch nach einem Fehlschlag: Der alte Buffer
   bleibt gültig, und der nächste Frame hat so trotzdem ein Render-Target.
5. Nur bei Erfolg: `width`/`height` setzen, `createDepth(w, h)` aufrufen und die
   Logzeile `D3D11Renderer: swapchain resized to WxH (client WxH)` schreiben.
   Die Swapchain-Größe darin wird per `GetDesc` zurückgelesen.

## Ursache 2: Use-after-free des Viewport-SRV (Treiber-Absturz)

Der Vorher-Stand stürzte bei Resizes ab, und zwar mit 0xC0000005 im
NVIDIA-Treiberthread (`nvwgf2umx.dll`, Lesezugriff auf 0x10 bzw. 0x298). Mit
Fix 1 allein blieb der Absturz bestehen und trat dann auch beim Verkleinern
auf. Die Swapchain war also nicht die Ursache.

Ursache ist die Reihenfolge im Editor (`ViewportPanel.cpp`):

1. `SetViewportSize(neu)` wird aufgerufen.
2. Gleich danach liefert `GetViewportTexture()` noch den **alten** SRV-Zeiger,
   und dieser landet als `ImTextureID` in der Drawdata dieses Frames.
3. `Render()` ruft dann `createViewportRT`, und dort wurde `viewportSRV` sofort
   freigegeben.
4. Danach bindet `ImGui_ImplDX11_RenderDrawData` den freigegebenen Zeiger.

Ob das abstürzt, hängt davon ab, ob der Speicher schon wiederverwendet ist.

OpenGL hat dafür `m_retiredTextures` („the current frame's ImGui draw list still
references the old GL texture id“), D3D11 hatte nichts Entsprechendes.

**Fix:** `createViewportRT` legt den alten SRV in `retiredViewportSRVs`, statt
ihn freizugeben. `Render()` leert die Liste am Anfang des **nächsten** Frames,
dessen Drawdata schon den neuen Zeiger trägt. Eine längere Frist braucht es
nicht: Ist ein View einmal gebunden, hält die D3D11-Runtime ihn selbst am Leben,
bis die GPU fertig ist. `RenderSceneImage` verschiebt das Live-Paar nur zur
Seite und legt es zurück; dabei wird nichts retired.

## Verifikation (NN-WS03, RTX 4070, Release, Anzeige 125 %)

**Laufzeit.** Skript: `docs/d3d11-swapchain-resize-verify.ps1`.

- Es startet den interaktiven Editor unter D3D11 mit eigenem APPDATA und
  eigenem Deploy (`C:\hw128`).
- Das Skript ist DPI-aware. Alle Größen sind daher physische Pixel; der Start
  liegt bei 2000×1125.
- Die Größe wird von außen per `SetWindowPos` gesetzt, ohne Aktivierung.
- Nach jedem Schritt nimmt `PrintWindow` den Client-Bereich auf.
- Gemessen werden die ersten UI-Kanten in der Bildmitte (`edgesY`) und die
  letzte nicht schwarze Zeile bei 90 % der Breite (`lastLitRow`).
- Minimieren und Wiederherstellen ist nicht gefahren: Das holt den
  Vordergrund, und der Rechner war in Benutzung.

| Schritt     | vorher (0513f8d4)          | nur Fix 1             | Fix 1 + 2 (3 Läufe, identisch) |
|-------------|----------------------------|-----------------------|--------------------------------|
| 2000×1125   | 1,23,24,46 / 1124          | 1,23,24,46 / 1124     | 1,23,24,46 / 1124              |
| 1280×720    | 1,29,30,73 / −1 (Ecke)     | 1,23,24,46 / 719      | 1,23,24,46 / 719               |
| 2300×1250   | Absturz                    | Absturz               | 1,23,24,46 / 1249              |
| 960×600     | –                          | –                     | 1,23,24,46 / 599               |
| nach 25 Zufallsgrößen, 1600×900 | – | –                     | 1,23,24,46 / 899               |
| zurück 1280×720 | –                      | –                     | 1,23,24,46 / 719               |
| Resize-Zeilen, davon Swapchain ≠ Client | 0 | 3, 0      | je 60, 0                       |

**Abstürze, nach Stand:**

- **Vorher:** 2 von 2 Läufen, beide beim Vergrößern. Einer davon war ein
  DPI-unaware Vorlauf.
- **Nur Fix 1:** 3 von 4 Läufen. Abgestürzt sind der volle Lauf, „direkt
  2300×1250“ und „1280×720 → 2000×1125“; der letzte schon beim *Verkleinern*.
  Nicht abgestürzt ist „2100×1180“.
- **Fix 1 + 2:** 0 von 6 Läufen. Das waren 3 volle Läufe, dieselben zwei
  Folgen, die vorher abstürzten, und ein Kurzlauf. Danach gab es keinen neuen
  `he_crash_*`-Report.

**Tests:**

- he_tests 4129/4129.
- Neuer Source-Pin „D3D11 main swapchain follows the window size (Thema 128)“
  in `tests/test_culling.cpp`, direkt hinter dem D3D12-Pin.
- Negativkontrollen:
  - Gegen die alte `D3D11Renderer.cpp` ist der Test rot.
  - Ohne die Retire-Zeile in `createViewportRT` ist er ebenfalls rot.

## Offen

- **Depth im Spielpfad.** Der Swapchain-Depth wird nur im exportierten Spiel
  benutzt (`!useViewport`). Der Editor-Lauf sieht ihn nicht; der Code ist
  derselbe wie bei D3D12, aber zur Laufzeit ist er unbelegt. Thema 112 hat
  dafür das Spiel exportiert (`d3d12-swapchain-resize-game-*.ps1`); dieselben
  Skripte mit `GameBackend=D3D11` sind der naheliegende nächste Schritt.
- **D3D11 hat keinen Debug-Layer-Schalter.** `HE_GPU_DEBUG` wirkt nur auf
  D3D12 und Vulkan, das Gerät wird ohne `D3D11_CREATE_DEVICE_DEBUG` erzeugt.
  Eine Debug-Layer-Zählung wie bei Thema 112 gibt es deshalb nicht. Ein
  Fehlschlag von `ResizeBuffers` würde mit `hr` als `[ERROR]` geloggt; in
  keinem Lauf kam eine solche Zeile vor.
- **Nicht getestet:**
  - Minimieren und Wiederherstellen; der 0×0-Zweig ist nur gelesen.
  - Ziehen am Fensterrand durch einen Menschen (modale Größenschleife).
  - Play-in-Editor.
