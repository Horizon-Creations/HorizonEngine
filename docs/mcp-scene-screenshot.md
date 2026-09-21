# MCP: Szenen-Screenshots aus einer Client-Kamera (Thema 74)

Stand 22.09.2026, Schritt 1 (Grundgerüst), Schritt 2 (Kamera je Client),
Schritt 3 (Kameras im Viewport) und Schritt 4 (Vollbau + Test mit zwei echten
Clients, §6) auf `claude/mcp-scene-screenshot-camera`. Was gebaut ist, wo die Nähte für die
Folgeschritte liegen, und was der Renderer dabei verspricht.

## 1. Das Werkzeug: `scene_screenshot`

Lesend (`mutates=false`), registriert in `EditorApplication::setupMcpTools`
über `registerScreenshotTools` (`src/HE_Editor/McpToolsScreenshot.cpp`).

Jeder verbundene Client hat **eine eigene Kamera** (`McpClientCamera`:
Position, Yaw, Pitch, FOV, Near/Far), die zwischen Aufrufen erhalten bleibt.
Der erste Aufruf mit irgendeinem Kamera-Argument legt sie an, Startpunkt ist
die Editor-Kamera; jeder weitere Aufruf geht von ihr aus. Absolute Argumente
ersetzen ihren Teil, relative werden danach angewandt, in der Reihenfolge
`turn`, dann `move`.

| Argument | Typ | Bedeutung |
|---|---|---|
| `position` | `[x,y,z]` | absolut. Kameraposition, Weltkoordinaten, +Y oben. |
| `look_at` | `[x,y,z]` | absolut. Zielpunkt; setzt Yaw und Pitch. Nicht zusammen mit `yaw`/`pitch`. |
| `yaw` | Grad | absolut. 0 blickt nach −Z, +90 nach +X (rechts). |
| `pitch` | −90..90 | absolut. 0 waagerecht, +90 senkrecht nach oben. |
| `fov` | 1..170 | absolut. vertikales Sichtfeld in Grad. |
| `turn` | `[Δyaw,Δpitch]` | relativ, Grad. Pitch stoppt am Pol, Yaw wickelt. |
| `move` | `[rechts,oben,vorn]` | relativ, Welteinheiten entlang der eigenen Kameraachsen, nach `turn`. |
| `reset` | bool | wirft die eigene Kamera weg; nur allein senden. |
| `render` | bool | Standard `true`. `false`: nur Kamera setzen, kein Bild. |
| `width`, `height` | 16..4096 | Zielgröße, Standard 1280×720, Budget 3840×2160 Pixel. |
| `output` | `inline` \| `file` | Standard `inline`. |
| `name` | `[A-Za-z0-9_-]{1,64}` | nur bei `file`: Basisname, `.png` wird angehängt. Fehlt: `scene_<ms>_<n>`. |

Ohne jedes Kamera-Argument: hat der Client eine Kamera, wird sie gerendert,
wie sie steht; hat er keine, ist das Bild die Sicht des Viewports, mit
Editor-Icons, und es wird **keine** Kamera angelegt („was der Mensch sieht"
gehört dem Client nicht). Sobald der Client eine eigene Kamera hat, sind die
Icons aus: er will die Szene, nicht die Editor-Hilfen.

Yaw/Pitch folgen exakt `EditorCamera::forward` (`(cp·sy, sp, −cp·cy)`), am
Pol dieselbe Up-Referenz wie `EditorCamera::upReference` (Karte, Norden
oben). Ein `look_at` senkrecht nach unten setzt Pitch −90 und lässt den Yaw
stehen, damit `turn` am Pol sinnvoll bleibt.

Die Kamera wird **vor** dem Render gespeichert: ein `unsupported` vom Backend
frisst die Bewegung nicht. Ein abgelehntes Argument (`invalid_args`) ändert
dagegen nichts.

Antwort (immer, in `structuredContent`):

```json
{ "width": 1280, "height": 720, "output": "inline", "rendered": true,
  "pngBytes": 412233, "backend": "Metal",
  "camera": { "position": [0,5,10], "lookAt": [0,5,9], "yaw": 0, "pitch": 0,
              "fov": 60, "fromViewport": false, "stored": true, "client": 3 },
  "path": "/Users/…/HorizonEngine/mcp-screenshots/scene_….png" }   // nur bei file
```

`lookAt` ist immer der Punkt eine Einheit voraus (die Kamera merkt sich eine
Richtung, nicht den Zielpunkt). `client` ist die Verbindungsnummer der Bridge;
`stored` sagt, ob das die eigene Kamera war.

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

* `tests/test_mcp_tools_screenshot.cpp`: 22 Fälle gegen einen Fake-Renderer
  (14 aus Schritt 1, 8 aus Schritt 2, dazu der base64-Fall)
  (Gradient rein, PNG raus, per `heLoadPngRGBA` pixelgenau zurückgelesen;
  Kamera aus position/look_at mit Vorwärtsvektor-Probe; Fallbacks;
  Dateiname-Einsperrung; Größen- und Inline-Budget; Ablehnungen; base64-Vektoren).
  Schritt 2 (per `McpTool::invoke(McpCallContext{id}, args)`): Kamera bleibt
  zwischen Aufrufen, zwei Clients halten zwei Kameras und `turn` des einen
  lässt den anderen stehen, ein dritter ohne Kamera sieht den Viewport ohne
  dass eine angelegt wird; `turn`/`move` relativ entlang der eigenen Achsen,
  Pol-Klemme und Yaw-Wrap; erster Aufruf startet bei der Viewport-Kamera;
  `render:false` speichert ohne Bild, ein gescheitertes Bild behält die
  Bewegung, ein abgelehntes Argument ändert nichts; `reset` und
  `notifyClientGone` löschen nur die eine Kamera, eine spätere gleiche Nummer
  startet leer; ohne Editor-Tabelle führt das Tool eine eigene;
  `McpClientCamera` Yaw/Pitch-Konvention und `lookAt`-Umkehrung.
* `tests/test_mcp_bridge.cpp`: „a tool's picture goes out as an MCP image
  block" (Reihenfolge, MIME, base64, keine Bytes bei Ablehnung). Schritt 2,
  über echten Loopback-Socket: „a tool learns which connection is calling,
  and hears when it is gone": zwei authentifizierte Clients bekommen zwei
  verschiedene, echte Ids (nie 0), `batch` reicht dieselbe Id an das innere
  Tool, das Auflegen des einen feuert den Client-Gone-Hook genau einmal mit
  dessen Id, `stop()` meldet den Rest.
* Echte Hardware: `HE_DUMP_SCENEIMAGE=<png>` in `dumpFrameHeadless`
  (`EditorApplication.cpp`) rendert nach dem normalen Dump (a) ein Still mit
  derselben Kamera und Größe, (b) ein Still um 90° gedreht bei 640×400, dann
  (c) noch einen echten Frame, und loggt: `sceneimage witness — same-camera
  still ok (X% px differ), turned still ok at 640x400 (mean rgb shift Y),
  live-after ok (Z% px differ from live-before)`. Erwartung: X und Z klein,
  Y deutlich. Aufruf: `scripts/he_shot.py /tmp/live.png SCENEIMAGE=/tmp/mcp.png`.

## 4. Kamera je Client (Schritt 2): wie die Identität zum Tool kommt

* **`McpCallContext { McpClientId client; }`** (`McpToolRegistry.h`). `McpTool`
  hat neben `handler(args)` eine zweite Signatur `handlerCtx(ctx, args)`;
  `McpTool::invoke(ctx, args)` nimmt die Kontext-Variante, wenn gesetzt.
  `McpToolRegistry::add` synthetisiert für ein Kontext-only-Tool den plain
  `handler` (Client 0 = anonym), damit `find(name)->handler(args)` in jedem
  Test und bei jedem älteren Aufrufer weiter funktioniert. `McpClientId` ist
  per `static_assert` in `McpBridge.cpp` derselbe Typ wie `HE::Net::ConnectionId`
  (uint32, ab 1 je Listener), der Registry-Header bleibt netzfrei.
* **Bridge:** `dispatch` ruft `tool->invoke(McpCallContext{id}, args)`; das
  Batch-Tool ist selbst `handlerCtx` und reicht den Aufrufer an die inneren
  Tools durch.
* **Aufräumen:** `McpToolRegistry::addClientGoneHook(fn)` /
  `notifyClientGone(id)`. Die Bridge meldet an **vier** Stellen: `Disconnected`
  (nur für zugelassene Verbindungen), beide Unauth-Drops, und in `stop()` für
  alle verbliebenen Clients, **nachdem** der Transport weg ist (danach wird
  nichts mehr gepollt, und der nächste Listener zählt wieder ab 1: ohne das
  erbte der erste Client des nächsten Starts die Kamera des vorigen).
* **`McpClientCameras`** (`src/HE_Editor/McpClientCameras.h`, header-only):
  `std::map<McpClientId, McpClientCamera>` mit `find/set/erase/all()`, in
  Id-Reihenfolge aufzählbar. Der Editor besitzt sie
  (`EditorApplication::m_mcpCameras`) und reicht sie per
  `McpScreenshotHooks::cameras` hinein; fehlt der Zeiger, führt das Tool eine
  private Tabelle (Tests). Der Client-Gone-Hook des Tools löscht daraus.

## 5. Die Kameras im Viewport (Schritt 3)

Jede Client-Kamera aus `m_mcpCameras` ist im Editor-Viewport zu sehen, in
zwei Hälften nach dem Muster der Collab-Präsenzmarker
(`src/HE_Editor/McpCameraGizmos.h/.cpp`):

* **Frustum, tiefengetestet** (`McpCameraGizmos::appendFrustums`): im
  Debug-Linien-Block von `EditorApplication::OnRender`, direkt nach dem
  Collab-Block, aber ohne dessen Session-Gate. 19 Linien je Kamera
  (`kLinesPerFrustum`): vier Kanten vom Auge zum fernen Rechteck, das ferne
  Rechteck doppelt (1,0/0,92, damit es auf unruhigem Hintergrund nicht
  verschwindet), ein nahes Rechteck bei einem Drittel, ein Up-Dreieck auf
  der Oberkante (Rolle lesbar). Öffnungswinkel = echtes `fovDeg`, Seitenverhältnis
  fest 16:9 (`kAspect`, das Default-Format des Tools; die Kamera speichert
  keins). Länge skaliert mit dem Abstand zur Editor-Kamera
  (`clamp(dist·0,08, 0,15, 6)`), also bildschirmkonstant wie die Collab-Ringe;
  steht der Betrachter in der Kamera, wird sie ausgelassen.
* **Tag über dem Bild** (`McpCameraGizmos::drawViewportLabels`): in
  `ViewportPanel` direkt neben `CollabPresenceBar::DrawViewportMarkers`, mit
  dessen `PlaceMarker` (außerhalb des Bildes an den Rand gepinnt, Pfeil
  zeigt die Richtung). Punkt in Client-Farbe plus dunkle Pille `MCP #<id>`;
  die Verbindungsnummer ist die einzige Identität, die die Bridge hat, und
  dieselbe Zahl, die die Tool-Antworten tragen. Der Viewport bekommt die
  Tabelle über `AppContext::mcpCameras` (nur lesen).
* **Farbe** (`colorFor`): Golden-Ratio-Hue wie `CollabController::
  participantColor`, aber um ein Drittel Rad versetzt, damit Client #1 und
  Teilnehmer #1 nicht gleich aussehen. Frustum und Tag nehmen dieselbe Farbe.
* **Schalter:** derselbe Show-Flag wie die Collab-Marker
  (`ShowFlags::collaborators`, Toolbar „Show → Collaborators"; Hilfetext in
  `EditorHelp.cpp` erweitert). Kein eigener Schalter: eine ferne Kamera ist
  eine ferne Kamera, ob Mensch oder Modell dahinter.
* **Echtzeit:** das Tool schreibt die Tabelle im Bridge-Pump (`m_mcp.update`,
  nach dem Debug-Block desselben Frames), das Gizmo zieht also im nächsten
  Frame nach, ein Frame hinter dem Tool-Aufruf.
* **Nicht im Screenshot:** `RenderSceneImage` fährt den normalen Frame samt
  Debug-Linien. Ohne Gegenmaßnahme säße das eigene Frustum eines Clients als
  Rahmen in seinem eigenen Bild (das innere 0,92-Rechteck, siehe Kontrollbild
  des Witness) und die Frustums der anderen machten sein Bild davon
  abhängig, wer sonst verbunden ist. Deshalb merkt sich `OnRender` den
  Bereich der Gizmo-Linien in `m_lastDebugLines`
  (`m_mcpGizmoLineBegin/End`), und der `renderImage`-Hook in
  `setupMcpTools` gibt dem Renderer für die Dauer des Captures die Liste ohne
  diesen Bereich und danach die volle zurück. Grid, Auswahl, Collider bleiben
  im Screenshot wie seit Schritt 1.

Verifikation:

* `tests/test_mcp_camera_gizmos.cpp`: 5 Fälle, reine Geometrie: alle Punkte
  vor dem Auge in Blickrichtung (Yaw 0 → -Z, Yaw 90 → +X, Pitch -90 → -Y),
  fernes Rechteck genau `length` voraus; Öffnung wächst mit `fov`, Unterkante
  bei `-length·tan(fov/2)`, Up-Dreieck darüber; drei Clients → 3×19 Linien
  in drei Farben (= `colorFor(id)`), Betrachter in einer Kamera → 2×19, leere
  Tabelle → 0; Länge verdoppelt sich mit dem Abstand, Klemmen 0,15/6;
  `labelFor`, 20 verschiedene Farben für die ersten 20 Ids.
* Echte Hardware (Metal): `HE_DUMP_MCPGIZMO=<png>` in `dumpFrameHeadless`
  sät Client 1 (0, 0.8, 0 → -Z) und Client 2 (0, 0.8, -3 → zurück auf Client
  1) durch das echte Tool, baut die Frustums wie `OnRender`, schreibt den
  Dump-Frame mit beiden (`<png>`) und nimmt drei Stills von Client 2 durch
  das Tool: mit Strip, ohne Strip (Kontrolle) und ganz ohne Linien
  (Referenz). Log: `mcpgizmo witness — cameras seeded (2 in table, 38
  lines), … client-2 still via tool ok (X% px differ from no-lines), control
  without strip ok (Y% px differ from no-lines)`. Erwartung: X ≈ 0, Y
  deutlich. Aufruf: `HE_SKY_TIME=30 python3 scripts/he_shot.py /tmp/live.png
  MCPGIZMO=/tmp/gizmo.png TOD=0.5 CAMX=3 CAMY=2 CAMZ=-1.5 YAW=-90 PITCH=-20`.
  Stand 21.09.2026: X = 0,00 %, Y = 1,67 %; im Dump-Frame zwei Frustums
  (magenta/cyan) mit Öffnungen aufeinander zu, in der Kontrolle das Frustum
  von Client 1 frontal plus der eigene cyanfarbene Rahmen, im Tool-Bild
  keine Linie. Ohne `HE_SKY_TIME` liegt X bei ~0,5 % (Wolkendrift zwischen
  den Stills). Die fünf Stills je Lauf gehen durch das echte Tool und landen
  daher in `~/Library/Application Support/HorizonEngine/mcp-screenshots/`,
  dem Ordner des Nutzers: nach dem Lauf wegräumen. Die ImGui-Tags sind headless nicht im Bild (der Dump läuft
  nicht durch das Viewport-Panel); real-HW-Sicht auf die Tags offen.

## 6. Zwei echte Clients gegen einen laufenden Editor (Schritt 4)

`scripts/he_mcp_multiclient.py OUTDIR [--live] [--probe]` ist der Beweis
für das Fertig-Kriterium des Themas, so nah am Ernstfall wie es ohne Menschen
geht: der deployte `HorizonEditor` wird mit `HE_MCP=1`, festem Port,
`HE_DUMP_RHI=Metal` und `HE_SKY_TIME=30` gestartet, unter einem **privaten
HOME** in OUTDIR (eigene `config.json`, eigene Endpunktdatei, eigener
`mcp-screenshots/`-Ordner; Config und Endpunkt des Menschen bleiben unberührt)
und mit einer Kopie des Tutorial-Projekts (`~/Documents/HorizonEngine/
HorizonTutorial`: Cube bei (0,1,0), Bodenplatte, Punktlicht) als
`LastProjectPath`. Die Clients sind `he_mcp.Bridge` aus dem echten Shim,
also derselbe Handshake und dieselbe Rahmung wie bei Claude.

Ablauf und Orakel (Stand 22.09.2026, alle grün):

1. **Sonde:** auth + `tools/list` in < 5 s, sonst Abbruch mit Ansage. Die
   Hauptschleife pumpt hier mit ~8–20 fps, `tools/list` 0,05 s.
2. **A** (Client 1) setzt `position (-2,1.5,3) look_at (0,1,0)`, **B**
   (Client 2) `position (3,1.5,-2) look_at (0,1,0)`; zwei verschiedene Ids,
   nie 0, beide `stored`; die Bilder unterscheiden sich in 72 % der Pixel.
3. **B übernimmt A's Zahlen** → B's Bild = A's Bild, **0,00 %** Pixel
   verschieden (das Bild folgt der Kamera, nicht der Verbindung); A's
   gespeicherte Kamera unverändert.
4. **A dreht** `turn [180,0]` (`render:false`), B's `render:false`-Antwort
   trägt dieselbe Kamera wie zuvor; A's nächstes Bild 73 % anders. Einmal
   `inline`: Bildblock nach dem Textblock, `image/png`, 125 kB bei 480×270.
5. **Live-Viewport** (`--live`): direkt nach Schritt 2 (`live_apart.png`)
   beide Frustums an ihren eigenen Plätzen, magenta #1 links des Cubes bei
   x ≈ 540 (aus der Editor-Kamera vorausberechnet 532), cyan #2 rechts bei
   x ≈ 1150 (vorausberechnet 1186), beide zum Cube geöffnet. Nach Schritt 4
   (`live_both.png`) beide bei (-2,1.5,3), weil B dort steht und A um 180°
   gedreht ist: cyan öffnet zum Cube, magenta davon weg. Kontrolle, dass die
   Aufnahme live ist: Cube per `entity_set_transform` verschoben → 1,9 %
   Pixel anders.
6. **A legt auf:** B's Kamera überlebt; ein Neuling C (Id 3, nicht A's 1)
   bekommt ohne Kamera-Argument `stored:false, fromViewport:true` und die
   Viewport-Kamera (6, 4.5, 6); die Editor-Tabelle meldet 1 Kamera; die
   Live-Aufnahme danach zeigt nur noch cyan (0,13 % Pixel anders, nur A's
   Frustum weg).

**Live-Viewport-Witness** (`HE_DUMP_LIVE=<bmp>` + `HE_DUMP_LIVE_TRIGGER=<datei>`,
`EditorApplication::captureLiveFrameIfAsked`, am Bridge-Pump): der
Headless-Dump läuft vor der Hauptschleife, wenn noch kein Client verbunden
sein kann; dieser Witness nimmt den **laufenden** Viewport auf, sobald die
Trigger-Datei auftaucht, und löscht sie erst nach dem Schreiben („Trigger
weg" = „Bild fertig"). Ein `stat()` pro Frame, nur wenn beide Variablen
gesetzt sind. Log: `live frame captured (1718x884, 2 MCP camera(s), gizmo
lines [0,38) of 1166, collaborators on, editor camera 6/4.5/6)`.

**Zwei Befunde, die der Lauf erst ans Licht gebracht hat:**

* **Metal: tiefengetestete Debug-Linien waren vor Geometrie unsichtbar.** Der
  Szenen-Pass rastert mit der unkorrigierten GL-Projektion (Tiefe = GL-ndc-z,
  so steht es am Deferred-Resolve und an `ssaoDepthPosFragment`),
  `EncodeDebugLines` aber mit `kMetalClipFix · viewProj` (z′ = 0,5·z + 0,5):
  jede Linie lag im Tiefenpuffer hinter jeder Geometrie, auch wenn sie
  räumlich davor stand. Sichtbar waren Linien nur gegen den Himmel, weshalb
  der Witness aus Schritt 3 (leere Welt) es nicht sehen konnte und der Grid
  in Projekten mit Bodenplatte „unter dem Boden verschwand". Fix: der
  Linien-Pass nimmt dieselbe Matrix wie die Szene (`MetalRenderer.mm`,
  `EncodeDebugLines`). Folge: Grid, Collider, Auswahl, Collab-Ringe und die
  MCP-Frustums sind auf Metal jetzt vor Meshes zu sehen; der Grid auf einer
  Bodenplatte bei y = 0 zeigt den erwartbaren Z-Fight-Stippel. Das ändert den
  Viewport jedes Metal-Nutzers und jedes Tool-Still (Grid im Bild), nicht nur
  dieses Thema. Ob D3D11/D3D12/Vulkan dieselbe Schieflage zwischen Szenen-
  und Linien-Pass haben, ist **nicht geprüft**.
* **`HE_DUMP_RHI` tauscht nur den Renderer, nicht `ctx.backend`.** Eine Config
  mit `RHI: 0` (OpenGL) plus `HE_DUMP_RHI=Metal` stürzt im ersten UI-Frame in
  `ImGui_ImplOpenGL3_NewFrame` → `glGetIntegerv` (SIGSEGV), weil
  `EditorUI::render` seinen ImGui-Backend-Zweig aus der Config wählt. Der
  Dump-Pfad beendet vor der UI und merkt nichts. Nicht gefixt (kein Teil des
  Themas), der Treiber schreibt `RHI: 4` in seine private Config.

**Einschränkungen, die bleiben:**

* `McpBridge::kMaxClients = 4`: laut Code wird der fünfte Client abgewiesen.
  **Nicht ausprobiert**, der Lauf hatte höchstens drei Clients zugleich.
* Jeder Screenshot ist ein voller Frame plus GPU-Readback, serialisiert auf
  dem Hauptthread im Bridge-Pump: gemessen 0,23–0,30 s bei 480×270
  (Debug-Build, zwei Clients). Viele Clients teilen sich diese Zeit und der
  Viewport steht währenddessen; **nicht gemessen**, wie sich vier Clients
  mit 1280×720 oder größer anfühlen.
* Inline-PNG ≤ 2,5 MiB (`kInlineMaxPngBytes`), Shim-Frame ≤ 4 MiB; darüber
  `file`.
* Nur Metal rendert Stills (`RenderSceneImage`); die Gizmos im Viewport gibt
  es auf jedem Backend, das Debug-Linien zeichnet.
* Die Frustums sind per Bauart in keinem Tool-Bild (§5); die ImGui-Tags
  (`MCP #n`) sind auch im Live-Witness nicht drin (der Capture liest die
  Viewport-Textur, nicht das ImGui-Overlay); Sicht auf die Tags bleibt
  Real-HW mit Mensch.
* Der Debug-Editor braucht auf macOS 27 ~6,5 min bis zur Endpunktdatei
  (Metal-Pipeline-Archiv dort deaktiviert, alles wird kompiliert); unter
  Last entsprechend mehr (`HE_SHOT_TIMEOUT`). Der Treiber verrät das Bild
  des Gizmos nur, wenn beide Client-Kameras im Blick der Editor-Kamera
  liegen (Start (6, 4.5, 6) → Ursprung).

## 7. Nähte für die Folgeschritte

* **Andere Backends:** OpenGL braucht denselben Umbau (Viewport-FBO
  beiseitelegen, Swapchain-Pass überspringen); D3D/Vulkan liefern bis dahin
  `unsupported` mit Namen.
