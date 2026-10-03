# Spielpfad-Postprocessing-Parity (D3D11 / D3D12 / Vulkan) — Analyse und Umfang

> Stand: 2026-10-03 · Thema 130, Schritt 1 (nur Analyse, kein Code).
> Auslöser: Doku 116 §2.1/§8 (`docs/deferred-d3d-vulkan-analysis-2026-10-01.md`, liegt auf
> Zweig `claude/deferred-rendering-d3d-vulkan`), Punkt A4 aus
> `docs/pr-followup-review-2026-10-02.md` (Zweig `claude/review-offene-prs-71-75-…`),
> Entscheidung des Menschen in Frage #13: Möglichkeit 1, Parity herstellen.
> Alle Zeilennummern beziehen sich auf `main` @ `9a1cc950`.

## 0. Kurzfassung

- Die Diagnose von Doku 116 §2.1 stimmt, und das Loch ist **größer** als dort genannt. Im
  Spielpfad fehlen auf allen drei Backends HDR, Tonemap (ACES und Gamma), Bloom, AA (FXAA,
  SMAA), TAA und SSR. Zusätzlich gilt:

  | Backend | SSAO im Spielpfad | GI im Spielpfad |
  |---|---|---|
  | D3D11 | **tot** | läuft |
  | D3D12 | läuft | läuft |
  | Vulkan | **tot** | **tot** |

  Details in §2.3, §3.2 und §4.2.
- **Das ausgelieferte Bild ist heute falsch kodiert, nicht nur „ohne Effekte“.** Gamma und
  Tonemap sitzen ausschließlich im Post-Pass (`D3D_Shared/HlslSources.h:415`,
  `shaders/postfx_tonemap.frag:11`). Der Szenen-Shader schreibt lineares, ungetonemapptes
  Licht direkt in ein UNORM-Backbuffer. D3D- und Vulkan-Spiele sind darum dunkler als im
  Editor, und Glanzlichter und Himmel clippen hart. Nach der Umstellung **ändert sich das
  Bild jedes ausgelieferten D3D- und Vulkan-Spiels sichtbar**, und zwar hin zum Editor-Bild.
  Das ist der Zweck der Änderung, gehört aber in die Release-Notes (siehe C6 im SSR-Plan:
  „vorlagepflichtig, weil es den Frame-Aufbau des gepackten Spiels ändert“; die Vorlage ist
  mit Frage #13 erledigt).
- Die Post-Kette muss **nicht portiert** werden. Sie existiert auf jedem der drei Backends
  vollständig als „Viewport-Frame“, und der Spielzweig ruft sie nur nicht auf. Empfohlen wird
  deshalb **Weg a**: Der Swapchain-Zweig fährt den Viewport-Frame in Swapchain-Größe und bringt
  das fertige LDR-Bild in den Backbuffer, auf D3D per `CopyResource`, auf Vulkan per
  Fullscreen-Pass (Details in §5). Weg b, ein zweiter Nachbau der Kette im Swapchain-Zweig, ist
  die Annahme hinter den „200–400 Z./Backend“. Er wäre Code-Duplikat mit eigenem Drift-Risiko.
- Die Spielseite braucht **nur einen Opt-in-Aufruf**. `GameApplication` gibt Bloom, SSAO,
  AA-Methode und -Schärfe, SSR, GI, DoF und RenderPath schon heute aus `config.json` an den
  Renderer weiter (`src/HE_Game/src/GameApplication.cpp:3160–3258`), die Werte verpuffen nur
  im Backend. `exposure` setzt auf D3D11 niemand, weder Editor noch Spiel; der Wert ist fest
  1.0, also gleich. Neu wäre ein Schalter, der dem Renderer sagt, dass der Swapchain-Zweig die
  Post-Kette fahren soll (§5.1). Ohne ihn kann das Backend „Spiel“ nicht von „Editor vor dem
  ersten `SetViewportSize`“ unterscheiden.
- Aufwand nach Weg a: **D3D11 ≈ 40–70, D3D12 ≈ 60–100, Vulkan ≈ 90–130 Zeilen**, dazu
  ≈ 15 Zeilen gemeinsam (Interface und Spiel). Die 200–400 Zeilen aus Doku 116 treffen nur Weg b
  und sind für Vulkan zu niedrig (Weg b ≈ 400–600). Herleitung in §6.
- Reihenfolge: **D3D11 → D3D12 → Vulkan**, nach steigender Komplexität (§7).

## 1. Die Verzweigung, die das Spiel aussperrt

`Render()` verzweigt auf allen drei Backends danach, ob ein Viewport-Ziel existiert. Das
Viewport-Ziel entsteht nur über `IRenderer::SetViewportSize`. Aufgerufen wird das von:

| Aufrufer | Stelle |
|---|---|
| Editor-Viewport-Panel | `src/HE_Editor/ViewportPanel.cpp:902` |
| Editor-Headless/Dump | `src/HE_Editor/EditorApplication.cpp:6297` |
| `HE_CAPTURE_FRAME` (auch im Spiel) | `src/HE_Core/src/Application/Application.cpp:569–570` |

`GameApplication` ruft es nie auf, also läuft das ausgelieferte Spiel immer durch den
Swapchain-Zweig. Nebenwirkung, die Verifikationen schon gestört hat: Ein Spiel mit
`HE_CAPTURE_FRAME` fährt den **Viewport**-Pfad und zeigt damit ein anderes Bild als der
Spieler (Memory „verify-exported-game-windows“). Nach Weg a gleichen sich beide Pfade an.

Referenz OpenGL: `OpenGLRenderer::DrawScene` hängt seine Post-Kette an jedes Ziel an, das
beim Aufruf gebunden ist. `Render()` (`OpenGLRenderer.cpp:12045–12092`) zeichnet im
Spielzweig mit `DrawScene(pw, ph)` auf FBO 0, **mit** voller Kette. Darum fiel das Loch auf
dem Standard-Backend nie auf.

## 2. D3D11 (selbst geprüft)

### 2.1 Ist-Zustand

`D3D11Renderer::Render()` steht in `D3D11Renderer.cpp:6892–6943`.

- Viewport-Zweig (`:6912`): `DrawViewportFrame()`, danach Clear des Backbuffers für ImGui.
- Swapchain-Zweig (`:6920–6938`): Backbuffer-RTV und Swapchain-Depth `p.dsv` binden, dann
  `p.taaFrame = false` (`:6934`, Kommentar „No post chain here“), `DrawScene(width, height)`
  direkt ins RGBA8-Backbuffer und `renderUIPass` auf das Backbuffer.
- Gemeinsam: `m_overlayCallback`, GPU-Timer, `Present`.

### 2.2 Was `DrawViewportFrame()` (`:6755–6890`) enthält: die komplette Kette

| # | Pass | Ziel / Ressource | Gate |
|---|---|---|---|
| 1 | TAA-Velocity-Clear, Jitter | `taaVelocityRTV` (`ensureTaaTargets`, `:1671f`) | `useHDR && aaMethod==TAA && taaReady()` (`:6773`) |
| 2 | Szene (inkl. GI, SSAO-Prepass, Refl-MRT, Forward-SSR in `DrawScene`) | `hdrRTV` RGBA16F + `viewportDSV` D24S8-typeless | `useHDR = postFxReady && hdrRTV && ldrRTV && viewportRTV` (`:6767`) |
| 3 | Bloom (Bright + Ping-Pong-Blur, halbe Auflösung) | `bloomTex[2]` RGBA16F | `bloomEnabled` |
| 4 | Tonemap (HDR + Bloom → LDR) | `ldrRTV` RGBA8 | `useHDR` |
| 5 | TAA-Resolve | `taaHistory[2]` RGBA8 | `taaFrame` |
| 6 | AA-Resolve (Blit / FXAA / SMAA / TAA-Sharpen) | **`viewportRTV` RGBA8** | immer, wenn `useHDR` |
| 7 | UI-Canvas | `viewportRTV` (`:6886–6888`) | immer |

Alle Zwischenziele baut ausschließlich `createViewportRT` (`:3755–3788`): Viewport-Farbe RGBA8
und Depth, dann `createHDRTargets` (`:2043ff`) für HDR, Bloom und LDR, darin
`createSSAOTargets` und die SSR-Vorframe-Kopie.

### 2.3 Was im Spielpfad stirbt und woran

- **HDR, Bloom, Tonemap, AA, TAA:** Die Passes werden gar nicht aufgerufen.
- **SSR:** `DrawScene` prüft, ob das gebundene RTV `hdrRTV` ist (`:6135–6150`, Kommentar
  „That is the C6 hole … needs its own decision“). Diese Entscheidung ist mit Frage #13 jetzt
  gefallen.
- **SSAO (neu, in Doku 116 nicht genannt):** `runSSAO` gibt sofort `whiteSRV` zurück, wenn
  `ssaoPosRTV` fehlt (`:2333`). `ssaoPosRTV` entsteht nur über `createHDRTargets →
  createSSAOTargets`. Das Spiel gibt `SSAOEnabled` also weiter, sieht aber nie AO.
- **GI** läuft dagegen auch im Spielpfad (`runGiShadow` legt seine Ziele lazy an, `ensureGiShadowTargets` `:3358`). Es hängt nicht am Viewport-Ziel, sondern an eigenen
  Half-Res-Zielen.
- `GetCapabilities()` (`:6945–6965`) meldet `supportsScreenSpaceReflections` und
  `supportsTemporalAA` über `postFxReady`, also **true**, auch im Spielpfad. Der Kommentar
  sagt das offen („the switch exists but does nothing“). Nach Weg a stimmt die Meldung.

### 2.4 Formate: Kopie statt Pass möglich

- Swapchain: `DXGI_FORMAT_R8G8B8A8_UNORM`, `BufferCount 1`, `DXGI_SWAP_EFFECT_DISCARD`
  (`:5481–5491`). Backbuffer über `swapchain->GetBuffer(0)` (`createRTV`, `:3815–3821`).
- Viewport-Endziel: `R8G8B8A8_UNORM`, DEFAULT, RT|SRV (`:3764`).
- Gleiches Format, gleiche Größe → `context->CopyResource(backbuffer, viewportTex)` ist
  zulässig. Es braucht weder Shader noch PSO noch sRGB-Frage. Beide Ziele sind UNORM, und der
  Tonemap-Shader schreibt bereits die Display-Kodierung, die der Editor zeigt.

### 2.5 Umbau nach Weg a (Skizze, kein Code in diesem Schritt)

Im Swapchain-Zweig, wenn `postFxReady`:

1. Ist `viewportW/H` ungleich `width/height`, `createViewportRT(width, height)` aufrufen. Das
   baut auch HDR, SSAO und SSR neu, wie im Editor-Resize.
2. `DrawViewportFrame()` statt `DrawScene()`. UI-Canvas, TAA-Gate und SSR-Gate sind dann
   automatisch richtig.
3. Den Backbuffer für die Kopie holen (`GetBuffer(0)`), `CopyResource`, danach das Backbuffer-RTV für
   `m_overlayCallback` binden.
4. Ohne `postFxReady` bleibt der alte direkte Pfad als Fallback.

Abzugrenzen ist ein „Spiel-Viewport“ vom Editor-Request (`viewportReqW/H`): Es darf
`GetViewportTexture()`/ImGui nichts anderes liefern als heute. Am einfachsten ist ein
internes Flag, `SetViewportSize` bleibt unberührt. Die Swapchain-Depth `p.dsv` wird im
Spielpfad dann nicht mehr gebraucht. Sie bleibt als Fallback bestehen, das kostet ein
Depth-Ziel in Fenstergröße.

Schätzung D3D11: **≈ 40–70 Zeilen** im Renderer. Dazu kommen Test und Zeuge (§8).

## 3. D3D12

Analyse durch einen Agenten. Die tragenden Stellen sind von Hand nachgeprüft (mit ✔ markiert).

### 3.1 Ist-Zustand

- `Render()` steht in `D3D12Renderer.cpp:10451–10638`. Ablauf:
  1. `resizeSwapchainIfNeeded()` (`:10489`).
  2. Viewport neu anlegen, wenn sich der Request geändert hat (`:10492–10494`).
  3. `useViewport` (`:10499`).
  4. Viewport-Zweig: `DrawViewportFrame()` (`:10525f`).
- Gemeinsamer Teil: Backbuffer PRESENT→RT, Backbuffer-RTV und Swapchain-DSV binden, beide
  clearen (`:10529–10542`).
- Swapchain-Block (`:10544–10563`): `usingHDR = false`, `taaFrame = false` (✔ `:10550f`),
  `DrawScene` direkt ins Backbuffer, `renderUIPass12` auf das Backbuffer.
- `DrawViewportFrame()` steht in `:10346–10449`:
  - HDR-Zweig (`:10353–10411`): TAA-Entscheidung und Velocity-Clear, `hdrRT` mit Viewport-DSV,
    `DrawScene`, `runPostFX`, UI-Canvas auf `viewportRT`. `viewportRT` bleibt danach in
    PIXEL_SHADER_RESOURCE.
  - LDR-Fallback (`:10412–10448`).
- `runPostFX` (`:2191–2337`) in dieser Reihenfolge: Bloom-Bright, 10 Blur-Passes, Tonemap →
  `ldrRT`, TAA-Resolve, AA (FXAA/SMAA/Blit/TAA-Sharpen). Das Ziel des AA-Passes ist
  **fest `viewportRT`**.
- Ressourcen aus `createPostFXResources` (`:1852–1951`), nur von `createViewportRT` aufgerufen
  (`:1664`): `hdrRT` RGBA16F, `bloomRT[2]` RGBA16F in halber Auflösung, `ldrRT` RGBA8,
  `postFxSrvHeap` mit 13 Slots, TAA-Ziele (RG16F Velocity, 2× RGBA8 History) und die
  SSR-Vorframe-Kopie `ssrColorHist` RGBA16F.

### 3.2 Was im Spielpfad stirbt

- **HDR, Bloom, Tonemap, AA, TAA:** Die Passes werden nicht aufgerufen.
- **SSR:** gesperrt über `usingHDR && hdrRT` (`:9696–9699`, Kommentar `:9688–9695`).
- **SSAO läuft** (anders als auf D3D11). Die Ziele entstehen lazy in `DrawScene`
  (✔ `:9710f`, `createSSAOTargets(width, height)`).
- **GI läuft** (`:9674–9686`), es hängt nicht am Viewport.
- `GetCapabilities()` (`:10640–10659`) meldet SSR und TAA mit dem Kommentar „the switch exists
  but does nothing“, wie D3D11.

### 3.3 Formate, Zustände, Lebensdauer

- **Backbuffer:** ✔ `R8G8B8A8_UNORM`, `FLIP_DISCARD`, `BufferCount = k_frameCount` (3),
  Usage `RENDER_TARGET_OUTPUT` (`:8893–8901`).
- **`viewportRT`:** ✔ `R8G8B8A8_UNORM` (`:1595`). Tonemap-, AA- und UI-PSOs sind alle
  RGBA8_UNORM. Das Backbuffer-RTV entsteht mit `nullptr`-Desc (✔ `:4084`, `:8922`), ist also UNORM; `_SRGB` gibt es nur bei Material-Texturen. Gamma kodiert der Tonemap-Shader selbst.
  → `CopyResource` kopiert byte-genau, eine sRGB-Frage stellt sich nicht.
- **Barrieren für die Kopie:**
  - `viewportRT`: PSR→COPY_SOURCE→PSR. Dabei `viewportState` mitführen.
  - Backbuffer: PRESENT→COPY_DEST→RENDER_TARGET (für eventuelle Overlays), danach wie bisher
    →PRESENT.
  - Das Clear des Backbuffers (`:10540`) und der Swapchain-Depth entfällt auf diesem Weg.
  - Kopieren in einen FLIP_DISCARD-Puffer ist zulässig. Es braucht trotzdem einmal einen Lauf
    mit Debug-Layer.
- **Lebensdauer:** `createViewportRT` (`:1570–1665`) flusht und gibt das alte Farbziel erst nach
  `k_frameCount + 2` Frames frei. `createPostFXResources` flusht erneut. SSAO, SSR und GI
  passen ihre Größe lazy an, jeweils mit eigenem Flush. Ein Fenster-Resize kostet im
  Spielpfad damit ≥ 3 Flushes. Das ist korrekt, aber ruckelig beim Ziehen; der Editor zahlt
  das heute schon.

### 3.4 Umbau nach Weg a

1. Nach `resizeSwapchainIfNeeded()` den Viewport an `p.width/p.height` binden. Das ist die
   physische Client-Größe und **nicht** `m_window->GetWidth()`: Bei 125 % DPI wären die Größen
   verschieden, und `CopyResource` braucht exakt gleiche Maße.
2. `DrawViewportFrame()` aufrufen, dann die Kopie mit den Barrieren aus §3.3.
3. Clear und `DrawScene` des Swapchain-Blocks überspringen.

Schätzung D3D12: **≈ 60–100 Zeilen**. Weg b wäre ≈ 250–400 Zeilen, weil `viewportDsvHeap`
und `viewportDepth` an vielen Stellen angenommen werden: `rebindSceneTarget` `:9494–9518`,
Decals `:8693–8711`, Debug-Linien `:10313`, TAA-Velocity-DSV `:10226`, SSR-Rebind `:10336`.

## 4. Vulkan

Analyse durch einen Agenten. Die tragenden Stellen sind von Hand nachgeprüft (mit ✔ markiert).

### 4.1 Ist-Zustand

- `Render()` steht in `VulkanRenderer.cpp:381–552`. Verzweigung über `useViewport`
  (✔ `:434f`). Ein Viewport-Resize wartet auf den Device-Idle und läuft vor dem Acquire
  (`:409–414`).
- Swapchain-Zweig (✔ `:481–493`): `m_taaFrame = false`, Shadow-Encode mit dem Aspect der
  Swapchain, Decal-Depth-Prepass. Danach öffnen beide Zweige `m_renderPass` (CLEAR, UNDEFINED →
  PRESENT_SRC). Nur im Spiel folgen darin `DrawScene(…)` mit LDR-Pipelines und der UI-Canvas
  (✔ `:504–516`). Dann Overlay, Submit, Present.
- `DrawViewportFrame(cmd)` steht in `:565–882`. `RenderSceneImage` nutzt die Funktion schon mit
  (`:4714`).
- Kette (alles unter `useHDR = m_postFxReady && m_hdrFB && m_ldrFB && m_fxaaFB`, ✔ `:572`):
  1. GI (`runGi`, ✔ `:606`)
  2. SSAO und Refl-MRT (`runSSAO`, ✔ `:623`)
  3. SSR-Trace (`RenderForwardSSR`, ✔ `:628`)
  4. Szene in RGBA16F (`m_postFxSceneRP`)
  5. TAA-Velocity
  6. SSR-History-Kopie
  7. Bloom (halbe Auflösung, Bright und 10 Blur-Passes)
  8. Tonemap → `m_ldrImage` RGBA8
  9. TAA-Resolve
  10. AA → `m_viewportImage` (`m_postFxFinalRP`)
  11. UI-Canvas (`m_uiViewportRP`)

  Größenabhängige Ziele entstehen in `createViewportResources` (`:4315–4480`), darin
  `createPostFXResources` (`:3769`) und `createSSAOTargets` (`:4478`).
- Die Szenen-Pipelines gibt es zweimal, als LDR gegen `m_renderPass` und als HDR gegen
  `m_postFxSceneRP`. Das gilt ebenso für Material, Himmel, Debug-Linien, Decals, Skinned und UI.
  `DrawScene` wählt die Variante über `hdr`.

### 4.2 Was im Spielpfad stirbt

- **Alles außer Schatten und Decals.** `runGi`, `runSSAO` und `RenderForwardSSR` werden nur
  aus `DrawViewportFrame` gerufen (✔ grep: einzige Aufrufer `:606/623/628`). Damit fehlen im
  Spiel auch **GI und SSAO**, obwohl `GetCapabilities()` `supportsGlobalIllumination = true`
  meldet und `GameApplication` GI daraufhin einschaltet.
- `GetCapabilities()` (`:884–910`) meldet `supportsHDR = false` hart. SSR wird über
  `m_postFxReady` gemeldet (Kommentar: „only in the editor viewport“), TAA über `taaReady()`.

### 4.3 Formate und der Weg ins Swapchain-Bild

- **Swapchain-Format** (✔ `:1201–1204`): `B8G8R8A8_UNORM`, sofern angeboten, **sonst
  `fmts[0]`**. Das kann ein `_SRGB`-Format sein und würde dann Gamma doppelt kodieren, sowohl
  beim Blit als auch beim Pass. Die Auswahl muss ein UNORM-Format bevorzugen und bei sRGB
  warnen oder linearisieren.
- **Swapchain-Usage** (✔ `:1218`): nur `COLOR_ATTACHMENT`, **kein `TRANSFER_DST`**.
- **`m_viewportImage`:** `R8G8B8A8_UNORM` mit `TRANSFER_SRC`, schon gammakodiert.
- `vkCmdCopyImage` RGBA→BGRA würde Rot und Blau tauschen. In Frage kommen deshalb nur zwei
  Wege:
  - **Blit** (`vkCmdBlitImage`). Braucht `TRANSFER_DST` auf der Swapchain (mit Prüfung von
    `supportedUsageFlags`) und eine zweite, kompatible `m_renderPass`-Variante mit LOAD statt
    CLEAR. Ohne sie löscht das CLEAR das geblittete Bild wieder.
  - **Fullscreen-Sampling-Pass in `m_renderPass`** (Empfehlung des Agenten und meine). Braucht
    eine Pipeline-Variante des Blit-Shaders gegen `m_renderPass` mit gesetztem
    `pDepthStencilState`, denn der Subpass hat Depth und `makePipe` (`:3619`) setzt keinen.
    Dazu kommt ein weiteres Descriptor-Set: Der PostFX-Pool ist mit ✔ `maxSets = 5` voll
    (`:3482–3490`). Außerdem nötig ist eine explizite Barriere COLOR_WRITE→SHADER_READ auf
    `m_viewportImage`, weil `m_postFxFinalRP` und `m_uiViewportRP` keine Subpass→EXTERNAL-
    Dependency haben und `runPostFXBarrier` bei gleichem Layout früh zurückkehrt (`:3442`).
    Vorteil: Format-Swizzle und eine spätere sRGB-Swapchain erledigen sich im Shader, und
    `m_uiPipeline` und ImGui bleiben im selben Pass.
- **Resize:** `recreateSwapchain` (`:1437`) wartet auf den Device-Idle. Das alte
  Viewport-Farbbild wird `k_maxFramesInFlight + 2` Frames zurückgehalten (`:4321–4328`). Im
  Spiel muss sich der Viewport an `m_swapExtent` orientieren, nicht an der Fenstergröße
  (DPI). Ein Recreate zur Present-Zeit fließt dann in den Viewport-Resize des nächsten Frames.

### 4.4 Shader-Auslieferung

Die Shader für PostFX, TAA, SSAO und GI liegen als vorkompilierte `.spv` vor
(`src/HE_Rendering/CMakeLists.txt:133–147`). Geladen werden sie aus `<exe>/Shaders` (`:1706–1711`),
ins Spiel kopiert nur über einen POST_BUILD-Schritt von HorizonGame
(`src/HE_Game/CMakeLists.txt:108–116`). Die SSR-Shader erzeugt shaderc zur Laufzeit.
Folgen:
- Eine reine Shader-Änderung braucht einen Relink (Memory „vulkan-spv-deploy-was-post-build“).
- Der Exporter lässt `Game/Shaders/` fallen. Der Fix `04de39bd` (Zweig
  `claude/d3d-und-evtl-vulkan-widgets-…`, Thema 133) ist ✔ **nicht in main**. Ohne ihn bleibt
  ein exportiertes Vulkan-Spiel aus anderem Grund schwarz.

### 4.5 Nebenbefund (nicht verifiziert)

Laut Agent bindet der LDR-Fallback von `DrawViewportFrame` (`:847–852`, ohne PostFX) Pipelines,
die gegen das BGRA8-`m_renderPass` gebaut sind, innerhalb des RGBA8-`m_viewportRenderPass`. Die
Formate sind nicht renderpass-kompatibel, das ergäbe einen Validierungsfehler. Er greift nur,
wenn die PostFX-Shader fehlen, und ist nicht Teil dieses Themas. Für Schritt Vulkan im Blick
behalten, weil Weg a im Fallback genau dorthin führt.

Schätzung Vulkan: **≈ 90–130 Zeilen**. Weg b ≈ 400–600 Zeilen: ein zweiter Satz
swapchain-großer Ziele, an `recreateSwapchain` gekoppelt, und die rund 300 Zeilen
`DrawViewportFrame` mit ihrer Größenkopplung an `m_viewportW/H` doppelt.

## 5. Weg a gegen Weg b

| | **a) Spielzweig fährt Viewport-Frame, dann Copy/Pass** | b) Post-Kette im Swapchain-Zweig nachbauen |
|---|---|---|
| Code | klein, nur Verdrahtung (§6) | 250–600 Z./Backend, Duplikat |
| Drift | keiner, Editor und Spiel nutzen einen Pfad | zwei Ketten, die auseinanderlaufen (die C6-Gates zeigen es schon) |
| SSAO/SSR/GI/TAA | kommen mit, weil sie am Viewport-Ziel hängen | jedes Gate einzeln umbauen |
| Laufzeitkosten | eine Vollbild-Kopie oder ein Pass pro Frame (RGBA8, klein), eine RGBA8-Fläche mehr; die Swapchain-Depth wird tot | keine Kopie |
| Risiko | Resize, Zustände/Barrieren (D3D12), Renderpass/Format (Vulkan) | alles aus a plus PSO- und Renderpass-Varianten |

Weg b spart nur die eine Kopie. Lohnt sich das später doch, kann der letzte AA-Pass unter Weg
a direkt ins Backbuffer schreiben. Auf D3D sind die Formate gleich, nur das Ziel von
`runPostFX`/AA wird zum Parameter. Das ist eine Optimierung von a, kein eigener Weg.

### 5.1 Opt-in statt Heuristik

Das Backend kann „Spiel“ nicht von „Editor vor dem ersten `SetViewportSize`“ unterscheiden,
denn `viewportReq` ist in beiden Fällen 0. Vorschlag:

- **Interface:** ein neuer Default-No-Op in `IRenderer`, etwa
  `virtual void SetSwapchainPostProcessing(bool)`. Er ist inline im HE_Core-Header, braucht
  also **kein** `HE_API` (siehe Memory zur HE_API-Konvention). GL und Metal ignorieren ihn,
  weil sie die Kette dort schon haben.
- **Aufrufer:** `GameApplication` setzt `true`, wenn eine Welt gerendert wird (Zweig
  `else if (r && m_world)`, `:3157`), und `false` im App-Modus.
- **App-Modus:** `GameApplication.cpp:3133–3156` schaltet die Kette für Anwendungen ohne Welt
  bewusst ab, weil sie über einer leeren Szene messbar Zeit kostete. Mit dem Schalter zahlt
  eine App weiterhin nichts.
- **Backend-intern:** Der Spiel-Viewport ist vom Editor-Request (`viewportReqW/H`) getrennt.
  `GetViewportTexture()` liefert dem Editor nichts Neues.

### 5.2 `HE_CAPTURE_FRAME` und der Zeuge

Heute schaltet `Application.cpp:569–570` ein Spiel mit `HE_CAPTURE_FRAME` auf den
Viewport-Pfad um. Das PPM zeigt dann das nachbearbeitete Bild, das **Fenster aber nur die
Clear-Farbe**. Captures waren deshalb nie ein Beleg für den Swapchain-Pfad (Memory
„verify-exported-game-windows“).

Nach Weg a liest `CaptureViewport` genau das Bild, das auch ins Backbuffer geht. Der Aufruf
von `SetViewportSize` in `Application.cpp` muss im Spielmodus dann entfallen oder vom Backend
ignoriert werden. Er nutzt `m_window->GetWidth()`, also logische Pixel, und würde bei 125 % DPI
die Größe gegen das Backbuffer verstellen. Danach ist `HE_CAPTURE_FRAME` ein **gültiger
Zeuge für das Spielbild**, und das Rezept aus PrintWindow und Zeitfenster bleibt nur als
Gegenprobe nötig.

## 6. Umfang je Backend

| | gemessen: Viewport-Frame | Weg a (Empfehlung) | Weg b (Doku-116-Annahme) |
|---|---|---|---|
| Gemeinsam (Interface + `GameApplication` + Capture-Hook) | — | ≈ 15 Z. | ≈ 15 Z. |
| D3D11 | `DrawViewportFrame` `:6755–6890`, 135 Z. | **≈ 40–70 Z.** | ≈ 200–300 Z. |
| D3D12 | `DrawViewportFrame` `:10346–10449` + `runPostFX` `:2191–2337`, ≈ 250 Z. | **≈ 60–100 Z.** | ≈ 250–400 Z. |
| Vulkan | `DrawViewportFrame` `:565–882`, 318 Z. | **≈ 90–130 Z.** | ≈ 400–600 Z. |

Herleitung Weg b: Die Kette wird dupliziert (Spanne in Spalte 2), dazu kommt das Umverdrahten
der Gates und Zielannahmen (§3.4, §4.3). Damit sind die 200–400 Z./Backend aus Doku 116 als
Größenordnung für b **bestätigt**, für Vulkan aber zu niedrig. Weg a liegt bei etwa einem
Viertel. Nicht eingerechnet: Tests und Zeugen-Skripte (§8) sowie der Fix des
Vulkan-Swapchain-Formats (§4.3, ≈ 10 Z.).

## 7. Reihenfolge

**D3D11 → D3D12 → Vulkan**, nach steigender Komplexität desselben Musters:

1. **D3D11:** keine Barrieren, keine PSO- oder Renderpass-Frage, `CopyResource` mit
   identischem Format. Das ist der Pilot, der das gemeinsame Interface (§5.1), den Capture-Hook
   (§5.2) und das Zeugen-Rezept festlegt.
2. **D3D12:** dasselbe Muster plus Ressourcenzustände, Flip-Model mit 3 Puffern und
   Flush-Kosten beim Resize.
3. **Vulkan:** Pipeline-Variante gegen `m_renderPass`, Descriptor-Set, Barriere, Wahl des
   Swapchain-Formats, Shader-Auslieferung (Vorbedingung `04de39bd`).

Eine fehlende Laufzeitprüfung ist **kein** Grund für diese Reihenfolge, anders als Doku 116 §7
für Deferred argumentierte. NN-WS03 hat eine RTX 4070, und das Rezept für exportierte Spiele
funktioniert auf allen drei Backends (Memory „headless-render-verification“, „verify-exported-
game-windows“). Jedes Backend ist ein eigener Schritt mit Bildbeleg und kann einzeln gemergt
werden.

## 8. Verifikation (je Backend-Schritt)

**Zeugen-Szene:** `docs/d3d12-swapchain-resize-witness-scene.ps1` als Basis, erweitert um ein
emissives Objekt über 1.0 (Bloom), eine glänzende Bodenfläche (SSR), feine Kanten (AA) und
eine Kamera-Bewegung (TAA). Export über die eigene MCP-Bridge (`project_package`).
`config.json` mit `GameBackend`, `GameWindowMode=Windowed`, `BloomEnabled`,
`AntiAliasing`, `SSREnabled`, `SSAOEnabled`.

1. **Positivbeleg:** Vorher-/Nachher-Bild des exportierten Spiels. Nach der Umstellung ist
   die Gamma-Anhebung messbar: Der Mittelwert der Mitteltöne steigt, Glanzlichter clippen
   nicht mehr. Bloom-Saum am Emissiv-Objekt.
2. **Parität zum Editor:** dieselbe Szene im Editor-Viewport (`HE_DUMP_*`) gegen das Spielbild
   bei gleicher Größe. Erwartet wird Gleichheit bis auf TAA-Rauschen und Zeitabhängiges
   (Himmel). Den Himmelsstreifen wie im Resize-Rezept ausmaskieren.
3. **Negativkontrollen:** `BloomEnabled=false` muss den Saum entfernen, `AntiAliasing=0` die
   Kantenglättung, `SSREnabled=false` die Spiegelung. Jede Kontrolle muss das Bild ändern,
   sonst misst der Zeuge nichts.
4. **Resize:** Bild nach dem Resize gegen einen Frischstart bei gleicher Größe (Rezept
   `d3d12-swapchain-resize-*`, 0 px Abweichung unterhalb des Himmels).
5. **Debug-Layer und Validierung:** D3D12-Debug-Layer (Kopie in FLIP_DISCARD) und
   Vulkan-Validierung ohne **neue** Meldungen gegenüber dem bekannten Grundrauschen
   (Memory „vulkan-d3d-baseline-noise“).
6. **App-Modus:** Ein exportiertes App-Projekt (`m_appMode`) zeigt unverändert die gleiche
   Frame-Zeit, der Schalter steht dort auf aus.
7. **Capture:** `HE_CAPTURE_FRAME` ergibt dasselbe Bild wie PrintWindow (§5.2).

Vorbedingungen:
- Für Vulkan muss `04de39bd` gemergt sein, oder `Game/Shaders` wird von Hand ins Export
  kopiert.
- Nach DLL-Änderungen HorizonGame und Editor neu linken (Memory „stale-dll-beside-tool-exes“),
  sonst testet der Export die alte DLL.
- ctest sieht davon nichts, denn der Pfad läuft nur im exportierten Spiel. Ein doctest kann
  höchstens den neuen Schalter am Interface prüfen.

## 9. Vorschlag für die Folgeschritte

1. D3D11 plus gemeinsames Interface, `GameApplication`-Opt-in und Capture-Hook, mit Zeuge
   §8 auf D3D11.
2. D3D12 mit Zeuge.
3. Vulkan inklusive Swapchain-Format-Fix, mit Zeuge. Vorbedingung `04de39bd`.
4. `GetCapabilities`-Kommentare (C6, „the switch exists but does nothing“) auf allen drei
   Backends bereinigen, `ssr-cross-backend-plan.md` C6 als erledigt markieren und
   Release-Notes-Hinweis zur Bildänderung (§0).

## 10. Ergebnis Schritt 2: D3D11 (2026-10-03)

Commit `567687ba`. Umgesetzt nach §2.5 und §5.1:

- `IRenderer::SetSwapchainPostProcessing(bool)`: Default-No-Op, als **letzte** virtuelle
  Funktion angehängt, damit die vtable-Slots davor bleiben, wo sie waren.
- `GameApplication` setzt `true` im Welt-Zweig und `false` im App-Modus, jeden Frame (der
  Setter speichert nur ein Flag).
- `D3D11Renderer::Render()`: Ist der Schalter an und `postFxReady`, läuft der Spielzweig über
  `DrawViewportFrame()` in Backbuffer-Größe und dann `CopyResource` ins Backbuffer. Die Größe
  kommt aus dem Backbuffer selbst (`rtv->GetResource` → `GetDesc`), nicht aus dem Fenster.
  Ein Editor-Request (`SetViewportSize`, z. B. vom `HE_CAPTURE_FRAME`-Hook mit logischer
  Größe) wird währenddessen ignoriert. Fällt der Schalter weg, wird das Spiel-Paar abgebaut,
  sonst liefe `Render()` in den Editor-Zweig mit schwarzem Clear. `Application.cpp` bleibt
  unverändert, weil D3D12 und Vulkan den Hook noch brauchen.
- Offen für Schritt 4 (§9): Die Kommentare zu C6 und „the switch exists but does nothing“
  in `DrawScene` (SSR-Gate), `GetCapabilities` (SSR/TAA) und am `SetSSRSettings` in
  `D3D11Renderer.h` sind für D3D11 jetzt überholt.

**Belege** (NN-WS03, RTX 4070, Release `C:/hw130`, Export über die eigene MCP-Bridge,
Szene Depthy aus Thema 112, Fenster 1600×900 physisch bei 125 % DPI). Skripte:
`docs/spielpfad-postfx-run-game.ps1` (PrintWindow, DPI-aware) und
`docs/spielpfad-postfx-cmp.ps1`. Gemessen unterhalb des Himmelsstreifens (obere 25 %):

| Vergleich | Ergebnis |
|---|---|
| Neu gegen Neu (Wiederholung) | 0 px Abweichung, Rauschboden 0 |
| Neu gegen Kontrolle (gleiche Exe, Schalter `false` = alter Pfad) | alle Pixel anders, mittlere Luma **202,1 statt 135,8** (Tonemap/Gamma) |
| SSAO an/aus, neu | 565 840 px anders |
| SSAO an/aus, alter Pfad | **0 px**: SSAO war im Spiel tot (§2.3) |
| Bloom an/aus, Schwelle 0,3 | mittlere Diff 15,9 |
| Bloom an/aus, Schwelle 1,0 | nur 208 px: Die Szene hat unter dem Himmel kaum HDR > 1 |
| SSR an/aus (MaxRoughness 1,0), neu | 369 643 px anders, Log „screen-space reflection pipeline created“ |
| SSR an/aus, alter Pfad | **0 px**: das C6-Gate hielt SSR im Spiel aus |
| AA FXAA gegen Off | 8 149 px anders (Kanten) |
| AA FXAA gegen TAA | 25 198 px anders, Log „TAA resolve active (1600x900)“ |
| Resize-Folge (Thema-112-Rezept) gegen Frischstart gleicher Größe | 0 px bei allen 7 Stufen, 1 px bei 1280×720 |
| `HE_CAPTURE_FRAME=120` gegen PrintWindow | 1600×900 (nicht der logische 1280×720-Request), 0 px Abweichung |

Log: „swapchain post chain active (WxH)“ einmal beim Start und einmal pro Größenwechsel,
nicht pro Frame. he_tests: 4165 Fälle, 3 fehlgeschlagen, alle Gamepad-End-to-End (bekanntes
Grundrauschen, echtes Xbox-Pad am Rechner).

Nicht gemessen: Parität Editor gegen Spiel als Bildvergleich (§8.2, strukturell gegeben, weil beide `DrawViewportFrame` fahren), Vollbild (der Default-`GameWindowMode`; nur Windowed getestet), Frame-Zeit-Kosten (volle Kette plus eine Kopie), ein App-Projekt (Schalter `false` ab Frame 1, also alter Code), der
Übergang `true` → `false` zur Laufzeit (kein realer Auslöser) und der D3D11-Debug-Layer.

## 11. Ergebnis Schritt 3: D3D12 (2026-10-03)

Commit `738e983b`. Umgesetzt nach §3.4, im selben Muster wie §10:

- `D3D12Renderer::SetSwapchainPostProcessing` speichert nur das Flag. `Render()` baut das
  Spiel-Set direkt nach `resizeSwapchainIfNeeded()` und **vor** `syncTaaTargets()` und dem
  Reset der Kommandoliste, denn `createViewportRT`, `createPostFXResources` und
  `syncTaaTargets` flushen. Größe und Format kommen aus dem Desc des aktuellen Backbuffers
  (`R8G8B8A8_UNORM`, 1×), nicht aus dem Fenster. Ein gescheitertes `ResizeBuffers` behält
  die alte Größe, und `CopyResource` braucht exakt gleiche Maße.
- Spielframe: `DrawViewportFrame()` (UI-Canvas inklusive), dann zwei Barrieren-Paare.
  `viewportRT` geht PSR→COPY_SOURCE→PSR, das Backbuffer PRESENT→COPY_DEST→RENDER_TARGET.
  Dazwischen läuft `CopyResource`. Das gemeinsame RT→PRESENT am Ende bleibt gültig. Clear,
  zweites `DrawScene` und `renderUIPass12` des alten Swapchain-Blocks entfallen; für das
  Overlay wird das Backbuffer-RTV ohne DSV gebunden.
- Ein Editor-Request (`HE_CAPTURE_FRAME`) wird währenddessen ignoriert. Fällt der Schalter
  weg, wird das Set nach einem `waitForAllFrames()` abgebaut, der Decal-Depth-SRV
  (`k_decalViewportDepthSlot`) bekommt eine Null-View (wie in `RenderSceneImage`), und die
  TAA-History wird ungültig.
- Offen für Schritt 4 (§9), überholt für D3D12: der C6-Kommentar am SSR-Gate in `DrawScene`
  (`D3D12Renderer.cpp:9696`), „the switch exists but does nothing“ in `GetCapabilities`
  (`:10738f`) und „Editor-viewport only … C6 hole“ an `SetSSRSettings` (`D3D12Renderer.h:76f`).

**Belege** (NN-WS03, RTX 4070, Release `C:/hw130`, Szene Depthy). Das Export-Paar liegt unter
`C:/hw130/s3`: `export_d3d12` ist der Depthy-Export aus Schritt 2 mit allen Top-Level-Dateien
aus dem frisch gebauten `deploy/Editor/Game` (also genau dem, was `project_package` kopiert;
Hash von `HorizonRendering.dll` gegen den Build geprüft). `export_ctl12` ist dieselbe Mappe
mit der Kontroll-`HorizonGame.exe` aus Schritt 2 (Opt-in `false`), läuft also mit **denselben
neuen DLLs** über den alten Pfad. Der D3D12-Spieler legt das Fenster DPI-skaliert an:
Config 1600×900 ergibt physisch 2000×1125, Config 1280×720 ergibt 1600×900. Skripte wie in §10, der
Runner hat jetzt `S2_GPUDEBUG` (setzt `HE_GPU_DEBUG`), der Vergleich `-File`. Gemessen
unterhalb des Himmelsstreifens (obere 25 %), 2000×1125, wo nicht anders angegeben:

| Vergleich | Ergebnis |
|---|---|
| Neu gegen Neu (Wiederholung) | 0 px Abweichung |
| Neu gegen Kontrolle (Opt-in `false`, alter Pfad) | alle Pixel anders, mittlere Luma **201,9 statt 133,9** (Tonemap/Gamma) |
| SSAO an/aus, neu | 1 042 002 px anders |
| SSAO an/aus, alter Pfad | 1 121 368 px anders: SSAO lief auf D3D12 schon im Spiel (§3.2), anders als auf D3D11 |
| Bloom an/aus, Schwelle 0,3 | mittlere Diff 10,9, Luma 213,1 gegen 201,9 |
| SSR an/aus (MaxRoughness 1,0), neu | 568 681 px anders, Log „screen-space reflection pipeline created“ |
| SSR an/aus, alter Pfad | **0 px**: das C6-Gate hielt SSR im Spiel aus |
| AA FXAA gegen Off | 10 131 px anders (Kanten) |
| AA FXAA gegen TAA | 32 173 px anders, Log „TAA resolve active (2000x1125)“ |
| Resize-Folge (Thema-112-Rezept, Start 1600×900 physisch) gegen Frischstart gleicher Größe | **0 px bei allen 8 Stufen** (1600×900, 1800×1000, 960×540, nach 30 schnellen Zufallsgrößen, langsam, minimiert/wiederhergestellt, 1280×720) |
| `HE_CAPTURE_FRAME=120` gegen PrintWindow (1600×900) | PPM 1600×900 = Backbuffer, nicht der logische 1280×720-Request; 0 px unter dem Himmel (im Himmel nur die animierten Wolken zwischen Frame 120 und dem späteren PrintWindow) |
| D3D12-Debug-Layer (`HE_GPU_DEBUG`, Log „GPU debug layer + DRED ENABLED“): neu FXAA / TAA+SSR / Resize-Folge mit TAA+SSR | 2 / 1 / 89 Meldungen, **alle** „ClearRenderTargetView: clear values do not match“ (Performance-Warnung), keine auf ERROR-Ebene |
| Dasselbe, Kontrolle (alter Pfad) fresh / Resize-Folge | 2 / 90 Meldungen derselben Art. Die Kopie und ihre Barrieren erzeugen **keine neue** Meldung |

Log: „swapchain post chain active (WxH)“ einmal beim Start und einmal pro Größenwechsel (Resize-Folge:
87 Zeilen bei 86 Swapchain-Resizes), nicht pro Frame; in der Kontrolle nie. he_tests (enthält das
D3D12-Backend nicht, Lauf nur als Stand): 4165 Fälle, 3 fehlgeschlagen, alle Gamepad-End-to-End
(bekanntes Grundrauschen).

Nicht gemessen: Parität Editor gegen Spiel als Bildbeleg (strukturell gegeben, beide fahren
`DrawViewportFrame`; die Luma 201,9 auf D3D12 gegen 202,1 auf D3D11 stammt aus verschiedenen
Fenstergrößen und ist kein Paritätsbeleg), Vollbild (nur Windowed), Frame-Zeit-Kosten (volle Kette,
eine Kopie und ≥ 3 Flushes pro Größenwechsel, §3.3), ein App-Projekt, und der Abbau-Pfad
`true` → `false` zur Laufzeit (kein realer Auslöser, im Code vorhanden, nie gelaufen).

## 12. Ergebnis Schritt 4: Vulkan (2026-10-03)

Commits `8ab0bb76` und `7e23f3d4`. Umgesetzt nach §4.3, Weg a, mit dem Vollbild-Pass statt
Kopie:

- `VulkanRenderer::SetSwapchainPostProcessing` speichert nur das Flag. `Render()` baut das
  Spiel-Set **vor dem Acquire** (nach `vkDeviceWaitIdle`) in `m_swapExtent`, also in
  physischen Pixeln, nicht in Fenstergröße. Ein Recreate bei Acquire oder Present greift im
  nächsten Frame. Ein Editor-Request (`HE_CAPTURE_FRAME`) wird währenddessen ignoriert.
- Spielframe: `DrawViewportFrame(cmd)` (GI, SSAO, SSR, Szene HDR, Bloom, Tonemap, TAA, AA,
  UI-Canvas). Dann eine explizite Barriere auf `m_viewportImage` (SHADER_READ→SHADER_READ,
  COLOR_ATTACHMENT_WRITE→SHADER_READ), weil `m_postFxFinalRP` und `m_uiViewportRP` keine
  Dependency nach EXTERNAL haben und `runPostFXBarrier` bei gleichem Layout nichts tut. In
  `m_renderPass` zeichnet ein Vollbild-Dreieck das Bild ins Swapchain-Bild:
  `postfx_aa_blit.frag` als neue Pipeline `m_presentPipe` gegen `m_renderPass` mit
  ausgeschalteter Tiefe, Set `m_presentDS` (PostFX-Pool 5→6 Sets). Das Set wird in
  `createPostFXResources` beschrieben und gilt damit auch nach dem Restore in
  `RenderSceneImage`. Kein neuer Shader, keine neue `.spv`. Die Vertauschung RGBA→BGRA
  erledigt der Schreibvorgang (`vkCmdCopyImage` würde Rot und Blau tauschen; ein Blit
  bräuchte `TRANSFER_DST` und eine LOAD-Variante von `m_renderPass`).
- Swapchain-Format: UNORM bevorzugt (`B8G8R8A8`, dann `R8G8B8A8`). Eine reine sRGB-Swapchain
  bleibt mit einer Warnung auf dem alten Pfad, sonst würde Gamma doppelt kodiert.
- Minimiertes Fenster (0×0-Surface): Das Set bleibt bestehen, und der Frame fällt wie vorher
  auf den direkten Pfad (`7e23f3d4`). Abgebaut wird nur, wenn das Spiel den Schalter
  zurücknimmt: nach einem `vkDeviceWaitIdle` `destroyViewportResources`, dann
  `pointSceneAoAtWhite()` (aus `RenderSceneImage` herausgezogen, denn Binding 3 zeigte auf das
  zerstörte SSAO-Ziel). TAA- und SSR-Historie werden ungültig.
- Offen für die Bereinigung (§9 Punkt 4), für Vulkan jetzt überholt: der Kommentar zu SSR
  „does not exist in the swapchain path at all“ in `DrawViewportFrame`, die Kommentare zu SSR
  „only in the editor viewport“ und TAA „The swapchain path renders unjittered“ in
  `GetCapabilities`, dort außerdem `supportsHDR = false`. `Application.cpp` (Capture-Hook)
  bleibt, weil GL und Metal ihn brauchen.

**Belege** (NN-WS03, RTX 4070, Release `C:/hw130`, Szene Depthy). Exportpaar unter
`C:/hw130/s4`: `export_vk` ist der Depthy-Export aus Schritt 3 mit allen Top-Level-Dateien
aus `deploy/Editor/Game`. **`Shaders/` ist von Hand kopiert** (42 `.spv`), weil `04de39bd`
nicht auf diesem Zweig ist (§4.4). `HorizonRendering.dll` wurde direkt aus
`src/HE_Rendering/` kopiert, weil die Kopie in `deploy/Editor/Game` nach dem DLL-only-Rebuild
veraltet war; der Hash ist gegen den Build geprüft. `export_ctlvk` ist dieselbe Mappe mit der
Kontroll-`HorizonGame.exe` aus Schritt 2 (Opt-in `false`) und **denselben neuen DLLs**.
`GameBackend=Vulkan`. Das Fenster ist DPI-skaliert wie auf D3D12: Config 1600×900 ergibt
2000×1125 physisch. Gemessen unterhalb des Himmelsstreifens, 2000×1125, wo nicht anders
angegeben. Alle Zeilen außer der Resize-Folge liefen mit der DLL aus `8ab0bb76`; die
Resize-Folge (neu und Kontrolle) mit der aus `7e23f3d4`, verglichen mit Frischstarts der
älteren DLL. Ein Frischstart läuft in beiden Ständen gleich, `7e23f3d4` ändert nur das
Minimieren.

| Vergleich | Ergebnis |
|---|---|
| Neu gegen Neu (Wiederholung) | 0 px Abweichung |
| Neu gegen Kontrolle (Opt-in `false`, alter Pfad) | alle Pixel anders, mittlere Luma **202,1 statt 135,8** (Tonemap/Gamma; dass D3D11 in §10 bei anderer Fenstergröße dieselben Zahlen zeigt, ist kein Paritätsbeleg, siehe §11) |
| SSAO an/aus, neu | 966 334 px anders |
| SSAO an/aus, alter Pfad | **0 px**: SSAO war im Vulkan-Spiel tot (§4.2) |
| GI an/aus, neu | alle Pixel anders, Luma 215,4 gegen 202,1, Log „GI pipelines built“ |
| GI an/aus, alter Pfad | **0 px**: GI war im Vulkan-Spiel tot (§4.2), obwohl `GetCapabilities` es meldet |
| SSR an/aus (MaxRoughness 1,0), neu | 568 683 px anders, Log „SSR pipelines created (forward, half-res trace)“ |
| SSR an/aus, alter Pfad | **0 px** |
| Bloom an/aus, Schwelle 0,3 | mittlere Diff 11,0, Luma 213,4 gegen 202,1 |
| AA FXAA gegen Off | 10 169 px anders (Kanten) |
| AA FXAA gegen TAA | 32 882 px anders, Log „TAA resolve active (2000x1125)“ |
| Resize-Folge (Thema-112-Rezept, Start 1600×900 physisch) gegen Frischstart gleicher Größe | **0 px bei allen 8 Stufen** (1600×900, 1800×1000, 960×540, nach 30 schnellen Zufallsgrößen, langsam, minimiert/wiederhergestellt, 1280×720) |
| `HE_CAPTURE_FRAME=120` gegen PrintWindow desselben Laufs (1600×900) | PPM 1600×900 (Swapchain, nicht der logische 1280×720-Request), **0 px** unter dem Himmel |
| Validierung (der Layer ist auf Vulkan immer an): neu gegen Kontrolle, frisch | in beiden nur `vkCmdUpdateBuffer` und `vkCmdPipelineBarrier` „inside an active VkRenderPass“, je bis zur Duplikatgrenze. **Keine neue VUID** |
| Validierung, Resize-Folge neu gegen Kontrolle | in beiden dieselben 0×0-Meldungen beim Minimieren (`vkCreateSwapchainKHR`, `vkCreateImage`, `vkCreateFramebuffer`, `vkCmdBeginRenderPass`): `recreateSwapchain` bei 0×0-Surface, älter als dieser Umbau |
| Validierung, SSR an | zusätzlich eine WARN „Vertex attribute at location 2 not consumed“ beim ersten Bau der SSR-Pipelines. Die Kontrolle baut sie nie; nicht aus dem neuen Code |
| Vulkan-**Editor** (`HE_DUMP_RHI=Vulkan`, `HE_DUMP_SSRTEST`, 16 Frames, Deploy `C:/hw130/deploy/Editor`) mit der neuen DLL gegen die DLL aus Schritt 3: Pool 5→6, `m_presentPipe`, Formatwahl laufen dort beim Start mit | exit 0 in allen drei Läufen; neu/neu 17 380 px (mittlere Diff 0,01), neu/alt 20 516 px (0,02), Luma beide 186,7: im Rauschbereich. Dieselben zwei Validierungsarten wie vorher, keine „swapchain post chain“-Zeile |

Log: „swapchain post chain active (WxH)“ einmal beim Start und einmal pro Größenwechsel
(Resize-Folge: 87 Zeilen; vor `7e23f3d4` waren es 88, weil Minimieren das Set ab- und wieder
aufbaute), in der Kontrolle nie. he_tests (enthält das Vulkan-Backend nicht, Lauf nur als
Stand, eigenes APPDATA): 4165 Fälle, 3 fehlgeschlagen, alle Gamepad-End-to-End (bekanntes
Grundrauschen).

Nicht gemessen: `RenderSceneImage` (MCP `scene_screenshot`) nach dem Herausziehen von
`pointSceneAoAtWhite`, eine reine sRGB-Swapchain (auf der RTX 4070 wird `B8G8R8A8_UNORM` angeboten;
der Zweig mit Warnung ist nie gelaufen), Parität Editor gegen Spiel als Bildbeleg (strukturell
gegeben, beide fahren `DrawViewportFrame`), Vollbild (nur Windowed), Frame-Zeit-Kosten (volle
Kette, ein Present-Pass, `vkDeviceWaitIdle` pro Größenwechsel), ein App-Projekt und der
Abbau-Pfad `true` → `false` zur Laufzeit (kein realer Auslöser, im Code vorhanden, nie
gelaufen).

## 13. Schritt 5: Prüfung vor dem PR (2026-10-03)

**main gemergt.** `04de39bd` (Thema 133, `Game/Shaders/` im Export) liegt über PR #82 in
`origin/main` (`git merge-base --is-ancestor`). `git merge-tree` meldete keinen Konflikt,
obwohl beide Seiten alle drei Renderer anfassen (main: UI-Stil, dieser Zweig: Spielpfad).
Gemergt als `ff0bef7d`, damit der Vulkan-Zeuge ohne Handkopie läuft.

**Build** (Release `C:/hw130`, `-j8`): 0 Fehler, einzige Warnung C5285 aus der
vendorten `doctest.h`. 105 laufzeitkompilierte Shader (51 HLSL, 54 GLSL) kompilieren.

**Falle im Build-Baum, nicht im Code:** Nach einem reinen DLL-Rebuild bleibt `deploy/Game`
(und damit `deploy/Editor/Game`, die Quelle des In-Editor-Exports) veraltet, weil es nur beim
Neulinken von `HorizonGame.exe` aufgefrischt wird. Hier lag dort noch `HorizonRendering.dll`
aus Schritt 4 und `HorizonCore.dll` aus Schritt 2. Erst nach Löschen von `HorizonGame.exe` und
`HorizonEditor.exe` und erneutem Build glichen die Laufzeit-DLLs dem Build (Hash geprüft).
Ein Voll-Build (CI, Release-Paket) ist davon nicht betroffen.

**Tests** (eigenes APPDATA): he_tests 4166 Fälle, 4163 bestanden, 3 fehlgeschlagen (11
Assertions, alle in `test_input_gamepad.cpp`, echtes Xbox-Pad am Rechner; gleiches Bild wie
in Schritt 2–4). ctest-Extras `editor_help_audit`, `he_mcp_shim`, `test_project_exporter`,
`test_d3d_shader_manager`, `runtime_size`: bestanden, die zwei App-Größentests übersprungen.

**Export-Zeuge auf dem gemergten Stand** (Depthy, Export über die MCP-Bridge des frischen
Editors, `C:/hw130/s5`, **nichts von Hand kopiert**): Export enthält `Shaders/` mit 40 `.spv`
(wie `deploy/Editor/Game/Shaders`). Frischstart 1600×900 Config = 2000×1125 physisch:

| Backend | Log | Bild unter dem Himmel |
|---|---|---|
| Vulkan | „swapchain post chain active (2000x1125)“, kein „scene shaders missing“; Validierung nur die zwei bekannten Basisfehler | **0 px** gegen Schritt 4 (`s4/shots/post`), Luma 202,1 |
| D3D12 | „swapchain post chain active (2000x1125)“, 0 ERROR | **0 px** gegen Schritt 3 (`s3/shots/post`), Luma 201,9 |
| D3D11 | „swapchain post chain active (2000x1125)“, 0 ERROR | Luma 201,9, D3D11 und D3D12 gegen Vulkan je mittlere Diff 0,31 |
