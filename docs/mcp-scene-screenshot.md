# MCP: Szenen-Screenshots aus einer Client-Kamera (Thema 74)

Stand 21.09.2026, Schritt 6 (Grundgerüst) auf `claude/mcp-scene-screenshot-camera`.
Was gebaut ist, wo die Nähte für die Folgeschritte liegen, und was der Renderer
dabei verspricht.

## 1. Das Werkzeug: `scene_screenshot`

Lesend (`mutates=false`), registriert in `EditorApplication::setupMcpTools`
über `registerScreenshotTools` (`src/HE_Editor/McpToolsScreenshot.cpp`).

| Argument | Typ | Bedeutung |
|---|---|---|
| `position` | `[x,y,z]` | Kameraposition, Weltkoordinaten, +Y oben. Fehlt: Position der Editor-Kamera. |
| `look_at` | `[x,y,z]` | Zielpunkt. Fehlt: Blickrichtung der Editor-Kamera (ohne Viewport: Ursprung). |
| `fov` | 1..170 | vertikales Sichtfeld in Grad. Fehlt: das des Viewports (sonst 60). |
| `width`, `height` | 16..4096 | Zielgröße, Standard 1280×720, Budget 3840×2160 Pixel. |
| `output` | `inline` \| `file` | Standard `inline`. |
| `name` | `[A-Za-z0-9_-]{1,64}` | nur bei `file`: Basisname, `.png` wird angehängt. Fehlt: `scene_<ms>_<n>`. |

Ohne jedes Kamera-Argument ist das Bild die Sicht des Viewports, mit
Editor-Icons. Sobald der Client eine Kamera beschreibt, sind die Icons aus:
er will die Szene, nicht die Editor-Hilfen.

Antwort (immer, in `structuredContent`):

```json
{ "width": 1280, "height": 720, "output": "inline", "pngBytes": 412233,
  "backend": "Metal",
  "camera": { "position": [0,5,10], "lookAt": [0,0,0], "fov": 60, "fromViewport": false },
  "path": "/Users/…/HorizonEngine/mcp-screenshots/scene_….png" }   // nur bei file
```

Bei `inline` hängt die Bridge zusätzlich einen MCP-Inhaltsblock
`{ "type": "image", "data": <base64>, "mimeType": "image/png" }` **nach** dem
Textblock an (`McpBridge::dispatch`). Das Modell sieht das Bild; die Bytes
stehen nie im JSON. Träger dafür ist `ToolResult::imageBytes/imageMime`
(`McpToolRegistry.h`), leer bei jedem anderen Werkzeug, nie bei einer
Ablehnung.

Ablehnungen: `no_world`, `no_camera` (keine Editor-Kamera und keine
`position`), `invalid_args`, `too_large` (Pixelbudget, oder inline-PNG über
`kInlineMaxPngBytes` = 2,5 MiB: das Shim `scripts/he_mcp.py` verweigert
Frames über 4 MiB, base64 wächst um ein Drittel), `no_directory`,
`unsupported` (Backend ohne Still-Pfad **oder** minimiertes Fenster; der
Backend-Name steht im Text), `encode_failed`, `write_failed`.

`file` schreibt ausschließlich nach `GlobalState::userDataDir()/mcp-screenshots/`
(neben der Endpunktdatei). Ein Client-Pfad wäre Dateizugriff, den
`McpToolRegistry.h` ausschließt. Das Batch-Werkzeug trägt pro Slot nur
`content`, ein Bild darin geht verloren; im Batch also `file` benutzen.

## 2. Der Renderer: `IRenderer::RenderSceneImage`

```cpp
virtual bool RenderSceneImage(const EditorCameraOverride& camera,
                              uint32_t width, uint32_t height,
                              std::vector<uint8_t>& rgba);   // RGBA8, oben zuerst
```

Vertrag (`IRenderer.h`): einmal die aktuelle Welt aus `camera` in exakt
`width`×`height` rendern und zurücklesen, ohne etwas zu präsentieren und ohne
dass der Live-Viewport danach anders aussieht. Basis-Implementierung liefert
`false` (→ `unsupported`). **Nur Metal** ist umgesetzt
(`MetalRenderer::RenderSceneImage`):

1. Live-Viewport-Texturen, `m_viewportReqW/H` und `m_editorCamera` werden
   beiseitegelegt (nicht retired), die Anforderung eingesetzt, `m_captureOnly`
   gesetzt.
2. Ein `EncodeFrame` läuft: Extraktion und Culling mit der Client-Kamera,
   alle Pässe wie im Viewport, aber **ohne Swapchain-Pass** (kein Drawable,
   kein ImGui, kein Present) und ohne Altern der Retire-Listen (die zählen in
   echten Frames).
3. `CaptureViewport` liest zurück (wartet auf die GPU), das Screenshot-Paar
   wird retired, das Live-Paar zurückgesetzt.
4. `m_taaHistoryValid = false` vor und nach dem Frame: der Screenshot mischt
   nicht gegen die Viewport-Historie, der Viewport nicht gegen die des
   Screenshots. Preis: je ein Frame unkonvergierte Kanten, kein Ghosting.

Was bewusst NICHT beiseitegelegt wird: HDR-, G-Buffer- und TAA-Targets. Sie
werden auf die Anforderung umgebaut und im nächsten echten Frame zurück. Das
ist ein Pfad pro Anfrage, keiner pro Frame.

## 3. Verifikation

* `tests/test_mcp_tools_screenshot.cpp`: 14 Fälle gegen einen Fake-Renderer
  (Gradient rein, PNG raus, per `heLoadPngRGBA` pixelgenau zurückgelesen;
  Kamera aus position/look_at mit Vorwärtsvektor-Probe; Fallbacks;
  Dateiname-Einsperrung; Größen- und Inline-Budget; Ablehnungen; base64-Vektoren).
* `tests/test_mcp_bridge.cpp`: „a tool's picture goes out as an MCP image
  block" (Reihenfolge, MIME, base64, keine Bytes bei Ablehnung).
* Echte Hardware: `HE_DUMP_SCENEIMAGE=<png>` in `dumpFrameHeadless`
  (`EditorApplication.cpp`) rendert nach dem normalen Dump (a) ein Still mit
  derselben Kamera und Größe, (b) ein Still um 90° gedreht bei 640×400, dann
  (c) noch einen echten Frame, und loggt: `sceneimage witness — same-camera
  still ok (X% px differ), turned still ok at 640x400 (mean rgb shift Y),
  live-after ok (Z% px differ from live-before)`. Erwartung: X und Z klein,
  Y deutlich. Aufruf: `scripts/he_shot.py /tmp/live.png SCENEIMAGE=/tmp/mcp.png`.

## 4. Nähte für die Folgeschritte

* **Kamera je Client (Schritt 2):** `McpScreenshotHooks::liveCamera` ist der
  einzige Rückfall, wenn der Client keine Kamera nennt. Eine Pro-Client-Kamera
  ersetzt genau diese Stelle. Dafür fehlt heute die Client-Identität im
  Handler: `McpBridge::dispatch` kennt die `ConnectionId`, ruft aber
  `tool->handler(args)`. Vorschlag: `McpTool::handler` um einen
  `McpCallContext { ConnectionId client; }` erweitern (oder eine zweite
  Handler-Signatur), und die Bridge meldet `Disconnected` an eine
  Aufräum-Hook, damit die Kamera mit der Verbindung stirbt.
* **Gizmos im Viewport (Schritt 3):** die Kameras sind dann Werte im Editor;
  `ViewportPanel` zeichnet Collab-Teilnehmer bereits als Frustum mit Kennung,
  dieselbe Zeichnung nimmt die Client-Kameras.
* **Andere Backends:** OpenGL braucht denselben Umbau (Viewport-FBO
  beiseitelegen, Swapchain-Pass überspringen); D3D/Vulkan liefern bis dahin
  `unsupported` mit Namen.
