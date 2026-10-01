# Deferred Rendering auf D3D11 / D3D12 / Vulkan — Analyse und Aufwand

> Stand: 2026-10-01 · Thema 116, Schritt 1 (nur Analyse, kein Code).
> Grundlage: `docs/deferred-renderer-plan.md` (P0–P7, Metal + GL), `docs/backend-parity-plan.md`
> (§1.4, P4), `docs/ssr-cross-backend-plan.md` (§2.2, C6) und der Code auf
> `claude/deferred-rendering-d3d-vulkan` = main `0513f8d4`. Zeilennummern beziehen sich darauf.

**Kurzfassung.** Die *Shader*-Seite von Deferred ist schon backend-neutral: Resolve aus derselben
`heLitP`-Quelle, G-Buffer-Emit-Tail der Graph-Materialien, Cluster-Scatter. Was D3D11, D3D12
und Vulkan fehlt, ist die *Pass-Verdrahtung*: G-Buffer-Targets, MRT-Pipelines, Depth-Übergabe
und Routing. Dazu kommt eine HLSL-Register-Pin-Tabelle, die der bestehende Plan übersehen hat
(§4.1). Der Code-Umfang liegt pro Backend etwa beim GL-Port (≈ 0,8–1,2 k Zeilen), die Zeit
steckt aber in der Verifikation: kein Backend davon läuft auf diesem Mac. Der funktionale
Hauptgewinn, Clustered Lighting für *Graph*-Materialien, ist **auch ohne Deferred** im
Forward-Pfad erreichbar und billiger. **Empfehlung:** erst die Forward-Lücken schließen (§7),
Deferred danach nur bei Bedarf, und dann D3D11 als Pilot.

---

## 1. Was GL und Metal tatsächlich haben

### 1.1 Pass-Struktur (beide Backends)

```
Shadow ▸ GI-Accel ▸ GI-Shadow ▸ GI-Probes
  ▸ G-Buffer-Pass   opak + masked (+ Instancing, Landscape); Translucent und Materialien
                    ohne G-Buffer-Variante werden in "deferredForward" gesammelt
  ▸ Decals          Projektor-Boxen blenden in GB0.rgb (vor der Beleuchtung)
  ▸ SSAO            Fullscreen aus der G-Buffer-Tiefe (P5), kein Geometrie-Prepass
  ▸ Resolve         Fullscreen, heLitP(G-Buffer) → HDR-Ziel
  ▸ Forward-Tail    deferredForward-Replay, Skinned (v1 forward!), Sky/Clouds, Transparenz
  ▸ PostFX          unverändert
```

GL: `OpenGLRenderer.cpp:10434` (Gate `deferredActive`), G-Buffer-Loop `:10793–11018`, Decals
`:11290–11368`, Resolve + Replay `:11372–11575`, Targets/Pipelines `:7310–7480`.
Metal: `EnsureGBufferTargets` `:10778`, `EnsureDeferredPipelines` `:10833`, `EncodeGBuffer`
`:15237` (≈ 370 Zeilen), `EncodeClusterData` `:13998`, `EncodeDeferredResolveTile` `:14160`,
`EncodeDecals` `:14302`.

### 1.2 G-Buffer-Layout (identisch GL/Metal)

| Target | Format | Inhalt |
|---|---|---|
| GB0 | RGBA8 sRGB | BaseColor.rgb, Metallic |
| GB1 | RGBA16F | Oct-Normale.rg, Roughness, Specular |
| GB2 | RGBA16F | Emissive.rgb (HDR), Material-AO |
| Depth | D24 (GL) / Szenen-Depth (Metal) | Weltposition per `invViewProj` rekonstruiert |
| GB3 | R32F | nur Metal-Tile (P6): NDC-Tiefe für Framebuffer-Fetch |

24 Byte/px (+4 auf Metal-Tile). Bei 2560×1440 ≈ 88 MB Schreiben + 88 MB Lesen pro Frame,
auf Apple Silicon nach P6 memoryless, also praktisch gratis.

### 1.3 Eine Shading-Quelle

Der Resolve ist **kein** eigener Lichtshader: `buildDeferredResolveSource`
(`MaterialShaderLibrary.cpp:966`) injiziert dieselbe `kLightingPreamble` wie jedes
Material-Fragment und ruft `heLitP(g0.rgb, N, g0.a, g1.b, P, g1.a, g2.a)`. CSM, Local-Atlas,
DDGI, GI-Masken, SkyEnv, AO, Wetter und Fog laufen damit per Konstruktion gleich wie im
Forward-Pfad. A/B-Gate auf Metal-HW: mittlere Abweichung 0,02–0,03/255.

Graph-Materialien bekommen einen zweiten Emit-Tail (`MatShaderGen::glslGBuffer` →
`MaterialAsset::customShaderGBufGlsl`, serialisiert, also auch in gepackten Builds).
Hand-GLSL ohne Graph wird automatisch forward geroutet.

### 1.4 Was nur Metal hat

- **P6 Tile/Single-Pass:** G-Buffer memoryless, Resolve per Framebuffer-Fetch. Auf
  Desktop-GPUs ohne Nutzen, auf D3D nicht abbildbar. Vulkan könnte es per Subpass/Input-
  Attachment nachbilden, das lohnt nur auf TBDR (Mobile, MoltenVK).
- **P7 Clustered im Resolve:** Punkt/Spot aus Cluster-Listen (16×9×24, 256 Lichter).
  GL-Deferred bleibt beim 8-Licht-Fenster (kein SSBO in GL 4.1). **„Deferred = viele
  Lichter" stimmt heute also nur auf Metal.**
- **SSR deferred** (lag-frei) und **Decals im Tile-G-Buffer**. GL hat Deferred-Decals, aber
  kein SSR-Deferred-Composite (A6 offen).

### 1.5 Umfang, gemessen

Commit `fecd85ed` (P0–P3, beide Backends, 31.07.): GL **+832**, Metal **+677**,
`MaterialShaderLibrary` +138 Zeilen. P5/P6/P7 je weitere ≈ 190–510 Zeilen
(`fd7e9472`, `dd434ee1`, `6b91d02b`). GL-Deferred ist **bis heute nie auf echter HW gelaufen**
(Sandbox ohne Display).

---

## 2. Was D3D11 / D3D12 / Vulkan heute schon haben

| Baustein | D3D11 | D3D12 | Vulkan | Beleg |
|---|:--:|:--:|:--:|---|
| Graph-Materialien via `heLitP` (Cross-Compile) | ja | ja | ja | `MaterialShaderLibrary::fragment`, HLSL-Pins `:2401–2485` |
| Forward-Clustered im **eingebauten** Szenen-Shader | ja (t18–t20) | ja (Root-SRVs) | ja (SSBO 10–12) | `HE::BuildClusterLights`, D3D11 `:3958` |
| Clustered für **Graph-Materialien** | — | — | — | `heLitP` hat keinen Cluster-Code (gap-audit Z. 431) |
| SSAO | Geometrie-Prepass | dto. | dto. | D3D11 `:5955` |
| SSR | forward (Refl-MRT-Prepass + Vorframe-HDR) | dto. | dto. | ssr-cross-backend-plan C/D/B |
| GI (SW-DDGI) | ja | ja | ja | eigener Welt-G-Buffer-Prepass (Half-Res) |
| TAA | ja | ja | ja | Capabilities |
| Decals | forward, selbst beleuchtet (1 Dir + Ambient) | dto. | dto. | `decalFragmentForward` |
| MRT-Pipelines vorhanden | ja (Refl-Prepass, 3 RT) | ja (`:5534`) | ja (GI-G-Buffer `:8868`) | |
| `RenderPath` ausgewertet | nein | nein | nein | `grep -c RenderPath` = 0 |
| HDR-Ziel | **nur Editor-Viewport** | **nur Editor-Viewport** | **nur Editor-Viewport** | siehe 2.1 |

### 2.1 Nebenbefund: der Spielpfad dieser drei Backends hat kein HDR/PostFX

`Render()` verzweigt auf allen drei Backends nach `useViewport` (D3D11 `:6729–6776`, D3D12
`:10408`, Vulkan `:431`). Den Viewport fordert nur der Editor an (`SetViewportSize`).
`GameApplication` ruft das nie auf, also läuft das gepackte Spiel durch den Swapchain-Zweig:
Szene direkt ins RGBA8-Backbuffer, **ohne HDR-Ziel, Tonemap, Bloom, AA, TAA und SSR**.
`ssr-cross-backend-plan.md` meldet das für D3D11 schon als „C6", es gilt aber für alle drei.

Einordnung: Standard-Backend für Spiele auf Windows/Linux ist **OpenGL**
(`RendererFactory::Default` `:89`, Export-Dialog `GameBackendRules.h:51` listet OpenGL zuerst).
Betroffen ist also, wer beim Export D3D11/D3D12/Vulkan wählt. Für Deferred heißt das: ein
Resolve könnte zwar ins selbe LDR-Ziel schreiben wie heute der Forward-Shader (gleiches Bild),
aber jeder Folgegewinn (SSR lag-frei, SSAO aus dem G-Buffer) setzt die PostFX-Kette voraus.

---

## 3. Wo Deferred auf diesen drei Backends wirklich etwas bringt

1. **Viele Lichter auf Graph-Materialien.** Der einzige *funktionale* Unterschied. Heute sieht
   ein Graph-Material auf D3D/Vulkan nur das 8-Licht-Fenster, der eingebaute PBR-Shader sieht
   schon 256 Cluster-Lichter. Ein Clustered-Resolve hebt das für alle G-Buffer-Materialien auf.
   **Aber:** dieselbe Wirkung erreicht ein Clustered-Zweig in `heLitP` selbst (Forward). Die
   Cluster-Puffer sind SRVs/SSBOs, keine Sampler, belasten also das volle s0–s15-Budget nicht.
   Das wäre ein Shader- plus Bind-Schritt pro Backend, keine neue Pass-Struktur, und er wirkte
   auch auf GL-4.3 und Metal-Forward.
2. **Weniger Geometrie-Durchläufe.** Forward rastert auf diesen Backends mit SSAO + SSR + GI
   bis zu dreimal zusätzlich (SSAO-Positions-Prepass, Refl-MRT, GI-Welt-G-Buffer). Ein
   G-Buffer könnte SSAO und SSR speisen (GL/Metal P5 haben das getan). Das ist ein echter, aber
   **ohne Windows-/Linux-HW nicht messbarer** Gewinn. Ohne Messung ist er nur eine Hoffnung.
3. **Beleuchtung nur einmal pro Pixel** (Overdraw). Relevant bei teuren Materialien und vielen
   überlappenden Flächen. Ebenfalls nur auf HW messbar, und auf Desktop-GPUs stehen dem
   ≈ 176 MB/Frame G-Buffer-Traffic bei 1440p gegenüber (P6-Trick gibt es dort nicht).
4. **Parität mit Metal bei Decals/SSR.** Decals würden echt beleuchtet (Schatten, Punktlichter,
   GI) statt mit dem eigenen 1-Dir+Ambient-Shading. Kosmetisch, aber sichtbar.

Was Deferred **nicht** bringt: Transparenz (bleibt forward), Skinned (läuft auch auf GL/Metal
v1 forward), MSAA (kein Thema, die Engine nutzt FXAA/SMAA/TAA).

---

## 4. Aufwand je Backend

### 4.1 Gemeinsam (einmal, in `MaterialShaderLibrary`)

- **HLSL-Pin-Tabelle für den Resolve. Korrektur zu `backend-parity-plan.md` §1.4/P4b:** dort
  steht, der Resolve werde „schon heute für jedes Backend übersetzt". Für HLSL stimmt das
  nicht. `compileResolveVariant` (`MaterialShaderLibrary.cpp:1182–1194`) schickt alles
  außer Metal **ungepinnt** durch `compile()`. SPIRV-Cross legt dann `binding = N` auf
  `register(sN/tN/bN)`, also G-Buffer 19–22 auf s19–s22, Preamble 16–18/31–33 auf s16+ und
  `HeResolve` auf **b23**. SM 5.0 endet bei s15 und b13. FXC lehnt das ab (X4509), genau
  der Fehler, den die Material-Fragmente bis zu ihrer Pin-Liste still hatten (Kommentar
  `:2403 ff.`).
  Lösung: Pin-Liste wie `kHlslMaterialPins`. Der Resolve bindet **keine** Graph-Texturen
  (heTex0, heTexP0–3, Landscape), also werden sechs Sampler frei, genug für GB0–2 + Tiefe.
  HeResolve auf ein freies b-Register (≤ b13). Die Gegenprobe wäre der bestehende
  Registertest in `test_material_graph.cpp` und FXC/WARP auf der Windows-CI.
- **G-Buffer-Fragment des eingebauten PBR-Shaders.** GL hat `kGBufFS`, Metal `kGBufferMSL`.
  Statt einer sechsten bis achten Handkopie: ein GLSL-Fragment in `MaterialShaderLibrary`,
  cross-kompiliert zu HLSL/SPIR-V (Muster `reflPrepassFragment`). ≈ 60–100 Zeilen.
- **Clustered-Variante öffnen** (`compileResolveVariant`, Sperre gilt nur GL 4.1). Prüfpunkt:
  SPIRV-Cross emittiert `readonly buffer` als **`ByteAddressBuffer`**, die vorhandenen
  Cluster-Puffer (t18–t20) sind als `StructuredBuffer` für den Hand-HLSL-Shader angelegt.
  Für t24–t26 also Raw-Views (`BUFFEREX_FLAG_RAW`) oder eine Compiler-Option, sonst bindet
  D3D11 still nichts.

### 4.2 Pro Backend (nach GL-Muster)

Was über GL hinausgeht, ist bei allen dreien dasselbe:

- **(a) Pipeline-Zustände:** MRT-Formate sind auf D3D12 und Vulkan Teil des PSO bzw. der
  Render-Pass-Kompatibilität. Jeder Material-PSO-Cache (`GetOrBuildMaterialPSO`
  D3D12 `:3224`, `GetOrBuildMaterialPipeline` Vulkan `:2446`) bekommt eine weitere
  Schlüsseldimension `gbuffer`, plus Warmup. D3D11 hat keine PSOs: dort reichen ein
  Pixel-Shader und ein Blend-State pro Variante, also die billigste Stelle.
- **(b) Vulkan zusätzlich:** eigener `VkRenderPass` + Framebuffer (3 Color + Depth),
  Layout-Übergänge Color→ShaderRead, Descriptor-Set-Layout mit Bindings 19–23 (und 24–26).
  Der Szenenpass muss die Tiefe mit `loadOp = LOAD` übernehmen oder kopieren.
- **(c) Depth-Übergabe** G-Buffer → Forward-Tail (Skinned, Sky, Transparenz, Debug, Gizmo).
  GL blittet, D3D11 `CopyResource`, D3D12 Barrier + Copy oder dieselbe Depth-Ressource
  mit Read-only-DSV, Vulkan siehe (b).
- **(d) Routing** im Draw-Loop (Translucent und Materialien ohne GB-Variante → deferredForward),
  Resolve-Lighting-Fill inkl. CSM-Matrizen (GL nutzt dafür einen eigenen UBO),
  `SetRenderPath`/Capabilities, `HE_DUMP_RENDERPATH`/`HE_DUMP_GBUFFER`.
- **(e) Optional danach:** SSAO aus G-Buffer-Tiefe (P5), Decals in GB0 (`decalFragmentSampled`
  existiert schon in HLSL mit Pins), Clustered-Resolve.

| | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| Kern (P0–P3-Äquivalent) | ≈ 700–900 Z. | ≈ 900–1 200 Z. (Root-Sig, PSO-Dimension, Barriers) | ≈ 1 000–1 300 Z. (RenderPass, Layouts, Descriptor-Sets) |
| Hive-Schritte Code | 2–3 | 3–4 | 3–4 |
| Laufzeit-Zeuge | **WARP in he_tests (CI Windows)** | WARP (CI Windows) | **keiner** (CI kompiliert nur; MoltenVK lokal nur Syntax) |
| Realistisch bis „verifiziert" | 1–1,5 Wochen | 1,5–2 Wochen | 2+ Wochen, plus Testinfrastruktur |

Vorbedingung in allen drei Fällen, wenn der Gewinn auch **im Spiel** ankommen soll:
HDR/PostFX im Swapchain-Zweig (§2.1). Das ist pro Backend ein eigener Schritt,
≈ 200–400 Zeilen: die Viewport-Kette auf das Backbuffer umleiten.

Die Zahlen sind Agenten-Zeit mit Review-Runden, keine Menschen-Wochen. Der Code-Anteil ist
klein, weil die Shader-Seite steht. Der Rest ist Verifikation: GL-Deferred zeigt, dass
„baut und Tests grün" ohne HW-Bild monatelang unbestätigt bleiben kann.

---

## 5. Risiken

| Risiko | Gegenmaßnahme |
|---|---|
| HLSL-Resolve scheitert still an FXC → Backend bleibt forward, niemand merkt es | Pin-Tabelle + Registertest **vor** dem Pass; Log-Gate „deferred path ready" wie Metal |
| D3D12-Root-Signatur deckt neue Register nicht → `E_INVALIDARG` beim PSO | WARP-PSO-Test wie bei Thema 56/57 |
| `ByteAddressBuffer` vs. `StructuredBuffer` bei Clustered | Puffer mit Raw-View anlegen, WARP-Pixeltest mit > 8 Lichtern |
| Zweite Wahrheit beim eingebauten PBR | G-Buffer-Fragment cross-kompiliert statt Handkopie |
| Vulkan ohne jeden Laufzeit-Zeugen | erst lavapipe/SwiftShader in CI (Linux) oder Vulkan über MoltenVK headless prüfen, sonst blind wie GL |
| Kein Perf-Nachweis möglich | Gewinn nicht versprechen, den Pfad nur als Option anbieten (Default bleibt Forward) |

---

## 6. Verifikationsplan (falls umgesetzt)

1. Registertest + FXC für den HLSL-Resolve (CI Windows), SPIR-V-Validierung (lokal).
2. **WARP-Pixel-A/B in he_tests**: dieselbe kleine Szene forward vs. deferred auf D3D11- und
   D3D12-WARP rendern, mittlere Abweichung < 1/255 (das P4-Gate aus dem Deferred-Plan).
   Damit wären D3D11/D3D12 **besser** verifiziert als GL-Deferred heute.
3. Vulkan: Laufzeit-Zeuge erst schaffen (lavapipe auf Linux-CI), dann dasselbe A/B.
4. Echte HW (Windows-PC eines Testers): Profiler-Capture forward vs. deferred in einer
   Szene mit vielen Lichtern, sonst bleibt §3.2/3.3 unbelegt.

---

## 7. Empfehlung

**Lohnt sich ein Vollport jetzt? Nein, nicht als Nächstes.** Begründung in einem Satz: der
einzige funktionale Gewinn (viele Lichter auf Graph-Materialien) ist im Forward-Pfad billiger
zu haben, die Perf-Gewinne sind ohne Windows-/Linux-HW nicht messbar, und der Spielpfad dieser
Backends hat noch nicht einmal die PostFX-Kette, an die Deferred anschließen würde.

Reihenfolge, wenn das Ziel „D3D/Vulkan auf Metal-Niveau" ist:

1. **Clustered in `heLitP` (Forward)** für alle Backends mit SSBO/Structured Buffers
   (D3D11, D3D12, Vulkan, GL 4.3, Metal-Forward). Schließt die sichtbarste Lücke mit dem
   kleinsten Eingriff. ≈ 1 Schritt Shader + 1 Schritt pro Backend für Bindings.
2. **HDR/PostFX im Swapchain-Zweig** von D3D11/D3D12/Vulkan (§2.1), *falls* D3D/Vulkan für
   ausgelieferte Spiele ernsthaft angeboten werden soll. Nützt Forward sofort.
3. **Deferred-Port**, wenn danach noch gewollt: gemeinsame Vorarbeit (§4.1), dann **D3D11
   als Pilot** (keine PSOs, WARP-Pixel-Zeuge), dann **D3D12** (gleiche HLSL + Pins, WARP),
   **Vulkan zuletzt** (erst Laufzeit-Zeuge schaffen). Das weicht von `backend-parity-plan.md`
   P4 ab („D3D12, Vulkan; D3D11 nach Maßgabe"): der Grund ist der Verifikationsweg, nicht
   der Code.
4. Tile/Single-Pass (P6) **nicht** portieren, Desktop-GPUs haben nichts davon.

## 8. Frage an den Menschen

Welche Option soll gelten?

- **A (empfohlen):** Forward-Lücken zuerst, also Clustered in `heLitP`, optional HDR/PostFX
  im Spielpfad. Deferred bleibt geparkt, bis es dafür einen messbaren Grund gibt.
- **B:** Deferred-Port jetzt, Reihenfolge D3D11 → D3D12 → Vulkan, gemeinsame Vorarbeit
  zuerst. Realistisch 4–6 Wochen Agentenzeit bis alle drei „verifiziert" sind, Vulkan
  davon nur mit neuer CI-Infrastruktur.
- **C:** Thema parken. D3D/Vulkan sind auf Windows/Linux nicht Standard (OpenGL ist es),
  und Metal/GL haben Deferred bereits.

Und eine Nebenfrage: Sollen D3D11/D3D12/Vulkan für **ausgelieferte Spiele** überhaupt
gleichwertig sein? Wenn nicht, ist §2.1 kein Fehler, sondern Absicht und gehört so in die
Export-Doku.
