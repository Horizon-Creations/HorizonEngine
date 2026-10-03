# Spielpfad-Postprocessing-Parity (D3D11 / D3D12 / Vulkan) — Analyse und Umfang

> Stand: 2026-10-03 · Thema 130, Schritt 1 (nur Analyse, kein Code).
> Auslöser: Doku 116 §2.1/§8 (`docs/deferred-d3d-vulkan-analysis-2026-10-01.md`, liegt auf
> Zweig `claude/deferred-rendering-d3d-vulkan`), Punkt A4 aus
> `docs/pr-followup-review-2026-10-02.md` (Zweig `claude/review-offene-prs-71-75-…`),
> Entscheidung des Menschen in Frage #13: Möglichkeit 1, Parity herstellen.
> Alle Zeilennummern beziehen sich auf `main` @ `9a1cc950`.

## 0. Kurzfassung

- Die Diagnose von Doku 116 §2.1 stimmt, und das Loch ist **größer** als dort genannt: Im
  Spielpfad fehlen auf allen drei Backends nicht nur HDR, Tonemap, Bloom, AA, TAA und SSR,
  sondern auch **SSAO** (D3D11 belegt, D3D12/Vulkan siehe §3/§4). Die SSAO-Ziele entstehen nur
  zusammen mit dem Viewport-Ziel.
- Die Post-Kette muss **nicht portiert** werden. Sie existiert auf jedem der drei Backends
  vollständig als „Viewport-Frame“, und der Spielzweig ruft sie nur nicht auf. Empfohlen wird
  deshalb: **Der Swapchain-Zweig fährt den Viewport-Frame in Fenstergröße und bringt das
  fertige LDR-Bild per Copy oder Blit in den Backbuffer** (Weg a, §5). Ein zweiter Nachbau der
  Kette im Swapchain-Zweig (Weg b, die Annahme hinter den „200–400 Z./Backend“) wäre
  Code-Duplikat mit eigenem Drift-Risiko.
- Auf der Spielseite ist **nichts** zu tun: `GameApplication` gibt Bloom, SSAO, AA-Methode,
  SSR, GI, DoF und RenderPath schon heute aus `config.json` an den Renderer weiter
  (`src/HE_Game/src/GameApplication.cpp:3160–3258`). Die Werte verpuffen nur im Backend.
- Aufwand nach Weg a: siehe Tabelle §6. Die 200–400 Zeilen aus Doku 116 gelten nur für Weg b.
- Reihenfolge: **D3D11 → D3D12 → Vulkan**, Begründung in §7.

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
- **GI** läuft dagegen auch im Spielpfad. Es hängt nicht am Viewport-Ziel, sondern an eigenen
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

_(wird nach Analyse ergänzt)_

## 4. Vulkan

_(wird nach Analyse ergänzt)_

## 5. Weg a gegen Weg b

| | **a) Spielzweig fährt Viewport-Frame + Copy/Blit** | b) Post-Kette im Swapchain-Zweig nachbauen |
|---|---|---|
| Code | klein, nur Verdrahtung | 200–400 Z./Backend (Doku 116), Duplikat |
| Drift | keiner, ein Pfad für Editor und Spiel | zwei Ketten, die auseinanderlaufen (wie die C6-Gates zeigen) |
| SSAO/SSR/TAA | kommen mit, weil sie am Viewport-Ziel hängen | jedes Gate einzeln umbauen |
| Kosten zur Laufzeit | eine Vollbild-Kopie pro Frame (RGBA8, vernachlässigbar), eine zusätzliche RGBA8-Fläche | keine Kopie, AA schreibt direkt ins Backbuffer |
| Risiko | Resize-Pfad, Ressourcenzustände bei D3D12/Vulkan | alles aus a plus PSO-/Renderpass-Varianten fürs Backbuffer-Format |

Weg b spart genau eine Kopie. Lohnt sich das später doch, kann der letzte AA-Pass unter Weg a
direkt ins Backbuffer schreiben, sobald die Formate passen. Das ist eine Optimierung auf Weg
a, kein eigener Weg.

**App-Modus (`m_appMode`)**: Für Anwendungen ohne Welt schaltet das Spiel Bloom, SSAO, AA,
GI und SSR aus und den Himmel ab (`GameApplication.cpp:3133–3156`), weil die Post-Kette über
einer leeren Szene messbar Zeit kostete. Unter Weg a würde ein App-Modus-Spiel trotzdem
HDR-Ziel, Tonemap und Kopie bezahlen. Vorschlag: Der Spielzweig nimmt den Viewport-Frame nur,
wenn eine Welt gerendert wird. Konkret: ein Renderer-Schalter („Spiel-PostFX an/aus“), den
`GameApplication` im App-Modus aus und sonst an setzt. Alternative: Gate im Backend auf „es
gibt eine Szene“. Das gehört in Schritt 2.

## 6. Umfang je Backend

_(wird nach D3D12-/Vulkan-Analyse ergänzt)_

## 7. Reihenfolge

_(wird ergänzt)_

## 8. Verifikation

_(wird ergänzt)_
