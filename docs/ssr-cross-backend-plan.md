# SSR: Cross-Backend-Port (Vulkan, D3D11, D3D12 — GL als Beifang)

Stand 06.09.2026, Branch `claude/ssr-cross-backend`. Schritt 1 des Themas:
Kartierung des bestehenden SSR-Pfads plus Portierungsplan. **Kein Feature-Code
in diesem Schritt.**

Bezug: `docs/ssr-plan.md` (der ursprüngliche Entwurf, P6 „andere Backends"),
`docs/backend-parity-plan.md` §1.3 und **P2** (dort ist diese Aufgabe schon
einmal aufgeschrieben worden — dieses Dokument prüft die Aussagen nach und
ersetzt sie durch aktuelle Fundstellen), `docs/decals-cross-backend-plan.md`
(das methodische Vorbild), `docs/deferred-renderer-plan.md`,
`docs/gi-reflections-plan.md`.

---

## 0. Die Korrektur vorweg

Das Thema sagt „GL+Metal fertig, es fehlen Vulkan/D3D11/D3D12". **Das stimmt
nicht.** SSR gibt es heute ausschließlich auf Metal.

Was GL hat, sind **ray-getracte GI-Reflexionen** (`docs/gi-reflections-plan.md`),
ein anderes Feature mit ähnlicher Wirkung: `m_giReflTex`, `kGiReflBlurFS`,
`kGiReflMixFS` in `OpenGLRenderer.cpp`. Der GL-Code sagt es selbst, an zwei
Stellen wörtlich:

> `OpenGLRenderer.cpp:5142` — „heSSRFwd stays unbound — **GL has no SSR pass**,
> and `heLight.ssr.x = 0` keeps that branch dead."
>
> `OpenGLRenderer.cpp:10530` — „`ssr[*]` stays zero — **GL has no screen-space
> trace**, so that branch of the same cascade folds away."

`docs/backend-parity-plan.md:216` führt die Zeile „SSR deferred / SSR forward"
für **alle vier** Nicht-Metal-Backends als `--`. Die Roadmap-Zahl von 85 % misst
also den Metal-Pfad, nicht die Backend-Abdeckung.

**Folge für den Auftrag:** es sind vier Backends zu verdrahten, nicht drei. GL
ist trotzdem kein stiller Scope-Zuwachs, sondern eine Entscheidung, die in §2.1
zur Vorlage kommt — mit dem Argument, dass GL das einzige Zielbackend ist, in dem
alle Vorbedingungen bereits vollständig liegen und das damit den Rahmen für die
anderen drei einfährt.

---

## 1. Der Metal-Pfad, wie er heute ist

SSR ist auf Metal **zwei** Pfade, die sich Shader und Zustandsmaschine teilen und
sich in Eingangsdaten und Composite unterscheiden.

### 1.1 Die Kette vom Schalter bis zum Pixel

| Stufe | Ort |
|---|---|
| Einstellungen (ABI) | `src/HE_Core/include/Renderer/IRenderer.h:355` (`SSRSettings`), `:364` (`SetSSRSettings`), `:162` (`supportsScreenSpaceReflections`) |
| Editor-UI + Persistenz | `src/HE_Editor/EditorSettingsPanel.cpp:346`, `EditorApplication.cpp:2292`, `:3521`, `EditorApplication.h:149` |
| Spiel-Laufzeit | `src/HE_Game/src/GameApplication.cpp:1558`, `:1564` |
| Einziges Backend, das den Setter überhaupt überschreibt | `src/HE_Rendering/include/Backends/Metal/MetalRenderer.h:123`, Rumpf `MetalRenderer.mm:14946`, Capability `:14925` |
| Shader (kanonisches GLSL 450, geteilt) | `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:1214` `kSSRTraceFS`, `:1467` `kSSRCompositeFS`, `:1577` `kSSRBlurFS`, `:1607` `kSSRRoughMixFS` |
| Accessoren (mit generischem `else`-Zweig) | `:1652` `ssrTrace`, `:1671` `ssrComposite`, `:1697` `ssrBlur`, `:1635` `ssrRoughMix` |
| Uniform-Layouts | `include/material/MaterialShaderLibrary.h:224` (`SSRTraceUniforms`), `:218` (`SSRBlurUniforms`) |
| Konsument im Shading (Forward) | `MaterialShaderLibrary.cpp:260` (`heSSRFwd`, Binding 31), Mix-Zweig `:568`; ABI-Feld `MaterialShaderLibrary.h:97` (`Lighting::ssr`) |
| Pipelines | `MetalRenderer.mm:12565` `EnsureSSRPipelines` (holt alle vier Shader `:12579-12582`) |
| Ziele | `:12660` `EnsureSSRTarget`, `:12684` `DestroySSRTarget` |
| Deferred-Pfad | `:12703` `EncodeSSRPasses`, Aufruf `:14187` |
| Forward-Pfad | `:12928` `EncodeForwardSSR`, Aufruf `:14225` |
| MRT-Prepass (Forward) | `:10097` `EnsureSSAOTargets` (legt `m_reflNormTex`/`m_reflDepthTex` mit an, `:10125-10131`), `:10151` `EncodeSSAO`, MRT-Zweig `:10215-10233`, Gate `:14073` |
| Prepass-Shader | ~~`MetalRenderer.mm:1364` `reflPosVertex`, `:1375` `reflPosFragment`, eingebettetes MSL~~ — **seit Schritt 3 in der geteilten Library**: `MaterialShaderLibrary.cpp` `kReflPrepassVS*`/`kReflPrepassFS`, Accessoren `reflPrepassVertex`/`reflPrepassFragment` |
| Vorframe-Farbkopie (Forward) | `MetalRenderer.h:255` `m_ssrColorHist` |
| Witness-Szenen | `EditorApplication.cpp:3509` (`HE_DUMP_SSR`, `HE_DUMP_SSRQUALITY`), `:4009` (`HE_DUMP_SSRTEST`), `:4022` (`HE_DUMP_SSRTESTROUGH`), `:4048` (`HE_DUMP_SSRTESTWALL`) |

### 1.2 Der deferred Pfad (`EncodeSSRPasses`)

Läuft, wenn der Deferred-Tile-Pfad aktiv ist. Eingaben sind der fertige
G-Buffer und die **aktuelle** aufgelöste HDR-Farbe — kein Vorframe, keine
Reprojektion, kein Ghosting:

```
G-Buffer ▸ Lighting-Resolve (ohne ambSpec, Flag heLight.ssr.w)
         ▸ Trace   (kSSRTraceFS → m_ssrHistRad[cur] + m_ssrHistPos[cur])
         ▸ Blur    (kSSRBlurFS, H/V über m_ssrPingTex)
         ▸ Wide    (zweite breite Stufe → m_ssrRoughTex, nur High)
         ▸ Composite (kSSRCompositeFS, ONE/ONE additiv auf die HDR-Farbe)
```

`kSSRCompositeFS` liest `heGB0/1/2` und `heGBDepth` (Bindings 19–22) plus die
vier Reflexionstexturen 27–30. **Er ist damit strikt an einen G-Buffer
gebunden.**

### 1.3 Der Forward-Pfad (`EncodeForwardSSR`)

Derselbe Trace, andere Eingaben und **kein** Composite-Pass:

- Ein **MRT-Prepass** hängt sich an den SSAO-Positions-Prepass und schreibt zwei
  weitere Attachments: `m_reflNormTex` (RGBA16F, oktaedrische Weltnormale in
  `rg`, **`b` = 0**) und `m_reflDepthTex` (R32F, NDC-Tiefe in
  `gbufferMain`-Konvention). Dadurch sieht der Trace exakt das Format, das er im
  Deferred-Pfad aus GB1/GBDepth bekäme.
- Die Radianz kommt aus `m_ssrColorHist`, einer Vollauflösungs-Kopie der
  HDR-Farbe des **letzten** Frames; der Treffer wird über `prevViewProj`
  dorthin reprojiziert. Umgeschaltet wird das im Shader über `cfg2.z = 1`.
- Der Konsument ist der Szenenshader selbst: `heLitP` liest `heSSRFwd`
  (Binding 31 → Metal-Slot 9), `fragmentMain` liest dieselbe Textur über
  `SceneUniforms::reflCfg/reflCfg2`. Der **Roughness-Fade passiert dort**, nicht
  im Trace — der Prepass hat keine Materialdaten, also ist `heGB1.b` in diesem
  Pfad konstant 0 und `roughFade` im Trace ein No-Op.
- `kSSRRoughMixFS` bäckt den Glossy-Lerp vorab in die eine Textur, die der
  Szenenshader sampelt.

**Preis:** ein Frame Inhaltsverzögerung (Option A aus `docs/ssr-plan.md` §2).

### 1.4 Die Konventionszahlen (`SSRTraceUniforms::conv`)

`conv = (ndc-y-Vorzeichen, Tiefen-Skalierung, Tiefen-Bias, Edge-Fade-Breite)`.
Metal setzt an beiden Aufrufstellen `(-1, 1, 0, 0.1)`. Das sind dieselben drei
Zahlen, die der Decal-Port als `params.yzw` gebraucht hat, und sie sind dort
schon für jedes Backend hergeleitet (`docs/decals-cross-backend-plan.md` §3 A2,
§6a, §6b, §6c) — sie werden **abgeschrieben, nicht neu abgeleitet**:

| Backend | y-Vorzeichen | Tiefen-Skalierung | Bias | Herkunft |
|---|:--:|:--:|:--:|---|
| Metal | −1 | 1 | 0 | heute im Code |
| D3D11 / D3D12 | −1 | 1 | 0 | Decal-Plan §6b/§6c |
| Vulkan | +1 | 1 | 0 | Decal-Plan §6a |
| OpenGL | +1 | 2 | −1 | Decal-Plan §3 A2 (`OpenGLRenderer.cpp` Resolve) |

---

## 2. Der Befund, der den Plan bestimmt

Vier Feststellungen, jede einzeln nachgeprüft.

### 2.1 Die Shader sind portabel — das bleibt richtig

`docs/backend-parity-plan.md` §1.3 behauptet, alle vier SSR-Shader lägen als
kanonisches GLSL 450 im geteilten Cross-Compiler und jeder Accessor habe einen
generischen `else`-Zweig auf `toTarget(backend)`. **Nachgeprüft: stimmt**, nur
sind die dortigen Zeilennummern rund 100 Zeilen veraltet (die Tabelle in §1.1
oben ist die aktuelle). Die dort protokollierte Handübersetzung durch
`he_shadercompiler` (HLSL SM5.0 / SPIR-V / GLSL 410, alle vier OK) ist damit die
belastbarste Einzelaussage, die dieses Thema hat.

**SSR auf D3D/Vulkan ist Verdrahtungsarbeit, keine Shader-Entwicklung.** Der
Aufwand liegt im C++-Rahmen.

Ein Nachtrag zu §1.3, den sie nicht macht: der **Prepass-Shader war nicht
portabel.** `reflPosVertex`/`reflPosFragment` waren eingebettetes MSL in
`MetalRenderer.mm`; der Trace war portabel, das, was ihn füttert, nicht.

> **Erledigt in Schritt 3.** Beide sind als kanonisches GLSL 450 in der
> geteilten Library (`reflPrepassVertex`/`reflPrepassFragment`), Metal baut
> seine Pipeline daraus, das MSL im Backend ist gelöscht. Der Vertex folgt
> derselben Aufteilung wie `standardVertex` — SSBO-Pull auf Metal (an
> `buffer(0)`/`buffer(1)` gepinnt, also genau die Bindepunkte, die der
> Prepass-Encoder ohnehin schon setzt), Attribute überall sonst, weil macOS-GL
> bei 4.1 kein SSBO hat. Der Oktaeder-Encoder ist zeichengleich der der
> Lighting-Preamble, damit `heOctDecode` im Trace beide Pfade bedient.

### 2.2 Kein G-Buffer außerhalb von Metal und GL — nur der Forward-Pfad ist portabel

Dasselbe Bild wie beim Decal-Port:

| Backend | Deferred-Pfad | HDR-Szenenziel | Konsequenz für SSR |
|---|---|---|---|
| **Metal** | vollständig (Tile) | ja | beide Pfade laufen |
| **OpenGL** | vollständig (Zwei-Pass) | `m_hdrColor` (`:7485`) | beide Pfade **möglich** |
| **Vulkan** | keiner | `m_hdrImage`, nur wenn `m_postFxReady` (`:363`) | nur Forward |
| **D3D11** | keiner | `hdrTex`, nur `useHDR` **und nur im Viewport-Pfad** (`:4502`) | nur Forward, mit Loch |
| **D3D12** | keiner | `hdrRT`, nur `useHDR` (`:7293`) | nur Forward |

`kSSRCompositeFS` ist damit für die drei Zielbackends **außer Reichweite** und
gehört nicht in ihren Scope. Das ist kein Verlust: der Forward-Pfad braucht ihn
nicht, er baut den Lerp per `kSSRRoughMixFS` in die Textur ein.

**Die Radianzquelle ist die eigentliche neue Vorbedingung.** Beim Decal-Port war
sie die lesbare Tiefe (Checkpoint C) — die ist inzwischen auf allen drei
Backends erledigt. Hier ist es die HDR-Szenenfarbe, und sie ist **an PostFX
gekoppelt**: ohne PostFX rendert die Szene direkt in ein RGBA8-Ziel, aus dem
sich keine HDR-Radianz lesen lässt.

> **D3D11 hat zusätzlich ein Loch:** `hdrRTV` entsteht in `createViewportRT` und
> wird nur im **Viewport**-Zweig von `Render()` benutzt (`:4494-4503`). Der
> Swapchain-Zweig (`:4577 ff.`, also das gepackte Spiel ohne Editor-Viewport)
> zeichnet direkt in den Backbuffer. Dort gibt es keine HDR-Farbe und damit
> keine SSR-Quelle, solange das so bleibt. Ehrlich melden, nicht heimlich
> umbauen.

### 2.3 Der Konsument ist zweiköpfig — und beide Köpfe fehlen

`docs/backend-parity-plan.md` P2d sagt, es sei nur `heLight.ssr` zu schreiben,
dann höre der bereits einkompilierte Forward-Konsument auf, wegkonstantengefaltet
zu werden. Das gilt für **einen** der beiden Köpfe.

**Kopf 1 — Graph-Materialien (`heLitP`).** Der Konsument ist tatsächlich in jedes
Backend einkompiliert (`MaterialShaderLibrary.cpp:260`, Mix-Zweig `:568`) und
wartet nur auf `heLight.ssr.x`. **Aber:** `heSSRFwd` liegt auf **Binding 31**.
SPIRV-Cross macht daraus `t31`/`s31`, und D3D11 hat eine harte Grenze von **16
Sampler-Slots pro Stufe** — `s31` kann die API nicht bedienen. Das ist exakt der
Befund, an dem der Decal-Port `he::shaderc::compileHlslPinned` gebaut hat
(`docs/decals-cross-backend-plan.md` §6b). Heute wird gepinnt **nur für Decals**
(`MaterialShaderLibrary.cpp:1849` `kDecalHlslPins`); die Preamble selbst ist
ungepinnt, und in ihr liegen außer 31/32 auch `heSkyEnv` (15), `heAO` (16),
`heGIIrradiance/Visibility` (17/18) und `heCloudShadow` (33). Ob D3D11 davon
heute etwas erreicht, ist eine bestehende Lücke und **kein SSR-Befund** — aber
sie steht zwischen dem SSR-Trace und den Graph-Materialien auf D3D11.

**Kopf 2 — die Built-in-Szenenshader.** Der zählt in `docs/backend-parity-plan.md`
gar nicht auf, und er ist der größere:

```
grep -c "uGIRefl|reflCfg|heGIRefl|SSRFwd"
  MetalRenderer.mm          14
  OpenGLRenderer.cpp        17
  D3D11Renderer.cpp          0
  D3D12Renderer.cpp          0
  shaders/scene.frag         0   (Vulkan)
```

Metals `fragmentMain` und GLs Szenen-GLSL haben die Reflexions-Kaskade
(Sky → GI-Refl → SSR). `kSceneHLSL` in beiden D3D-Backends und Vulkans
`scene.frag` haben **keine einzige Zeile davon**. Für sie ist P2d keine
ABI-Füllung, sondern eine Handportierung des Composite-Ausdrucks samt
per-Pixel-Roughness-Fade in drei weitere Kopien der Shading-Mathematik.

**Für `scene.frag` kehrt das eine dokumentierte Entscheidung um.** Zeile 46 dort
trägt ein Drift-Banner: „`ssr-plan` P4 — bekannter Drift, dokumentieren statt
portieren." Diese Phase würde das umdrehen. Genau wie bei P3 im Parity-Plan gilt:
**bewusste Änderung, keine Übersehung** — und sie gehört vor den Code, nicht
hinein.

### 2.4 Es gab keinen einzigen SSR-Test

`grep -l "ssrTrace\|ssrBlur\|ssrComposite\|ssrRoughMix" tests/` fand **nichts**.
Die vier Accessoren waren durch keinen automatischen Test gedeckt; der
generische `else`-Zweig wurde außerhalb der Handprüfung in
`docs/backend-parity-plan.md` §1.3 nie ausgeführt. Dasselbe Loch, das der
Decal-Port vorfand (§7 dort: „Vor Schritt 2 gab es keinen einzigen Decal-Test").

> **Geschlossen in Schritt 2.** `tests/test_material_graph.cpp`, „SSR shaders
> cross-compile for every backend" (vier Accessoren × Metal/GLSL410/HLSL/SpirV,
> alle vier grün) und „The reflection pre-pass is one shader for all backends".
> Der generische `else`-Zweig läuft damit zum ersten Mal automatisch.

---

## 3. Die Gabelungen — Entscheidungen, die vor den Code gehören

Drei Stück. Sie stehen hier, damit sie sichtbar entschieden werden; dieser
Schritt entscheidet sie nicht.

### 3.1 Ist GL im Scope?

Das Thema nennt Vulkan/D3D11/D3D12. §0 zeigt, dass GL SSR ebenfalls nicht hat.

**Empfehlung: ja, und zwar zuerst.** GL ist das einzige Zielbackend, in dem alle
Vorbedingungen bereits liegen — ein vollständiger Deferred-Pfad, `m_hdrColor`,
ein Reflexions-Prepass, der Normalen **und Roughness** schreibt
(`m_giGBufPosTex`/`NormTex`/`MatTex`, `OpenGLRenderer.cpp:6489-6518`), die
Reflexions-Kaskade im Szenenshader und `heGIReflFwd` auf Unit 18. Auf GL ist SSR
näherungsweise „noch eine Textur in eine Kaskade, die schon steht"; und GL ist
das Backend, gegen das sich der geteilte Trace hier offline am billigsten prüfen
lässt. Was dort einmal richtig verdrahtet ist, ist die Vorlage für die anderen
drei.

Wer nur D3D und Vulkan will, lässt Checkpoint A weg — dann fällt aber die
Vorlage weg.

### 3.2 Woher kommen die Normalen auf VK/D3D?

Die Tiefe ist **erledigt**: der Decal-Port hat sie auf D3D11 (`R24G8_TYPELESS`
mit DSV+SRV) und D3D12 (`R32_TYPELESS`) sampelbar gemacht und auf Vulkan einen
Kamera-Tiefen-Vorpass in ein eigenes Bild gebaut (`EncodeDecalDepth`). Was fehlt,
sind oktaedrische Normalen im GB1-Format.

**Weg (a) — Metals Weg: den SSAO-Prepass auf MRT erweitern.** Alle drei Backends
rasterisieren dort ohnehin schon die Geometrie (D3D11 `ssaoPosTex`, D3D12 drei
RTVs im `ssaoRtvHeap`, Vulkan `m_ssaoPosRenderPass`), die zusätzlichen
Attachments sind also fast umsonst. Echte Vertex-Normalen, keine Kanten-Artefakte.
Preis: `reflPosVertex`/`reflPosFragment` müssen nach kanonischem GLSL 450 in die
geteilte Library gehoben werden (wie `kDecalVS`/`kDecalFS`), sonst schreibt jedes
Backend seine eigene Kopie und die vier laufen auseinander.

**Weg (b) — Normalen aus der Tiefe rekonstruieren.** Ein Fullscreen-Pass mit
`cross(ddx, ddy)`, wie es der SSAO-Shader längst tut. Keine Geometrie, kein
neuer Prepass. Preis: facettierte Normalen an Kanten — und der Trace
punkt-sampelt GB1 genau deshalb, weil gelerpte Oct-Normalen dort zu Müll
dekodieren (`MetalRenderer.mm:13002`, Kommentar). Eine Spiegelung ist auf
Normalfehler deutlich empfindlicher als eine Verdeckung.

**Empfehlung: (a), mit dem Prepass-Shader in der geteilten Library.** Das macht
aus vier Kopien eine Quelle und ist zugleich der Beifang, der GLs und Metals
Prepässe später zusammenführen kann.

### 3.3 Umkehr des `scene.frag`-Drift-Banners

Siehe §2.3, Kopf 2. Ohne diese Umkehr bekommen die Built-in-Materialien auf
Vulkan keine Reflexion, und SSR wäre dort auf Graph-Materialien beschränkt (die
nach §2.3 Kopf 1 auf D3D11 ihrerseits an der Registergrenze hängen). Beides
zusammen hieße: SSR läuft, ist aber nirgends sichtbar. **Das ist der Punkt, an
dem diese Phase kippt oder trägt.**

---

## 4. Checkpoint A — OpenGL (die Vorlage)

Reihenfolge-Argument wie beim Decal-Port §4: der Pfad, der sich hier am
billigsten prüfen lässt, gehört nach vorn.

- **A1 · `SetSSRSettings`-Override + Capability.** Heute überschreibt außer
  Metal kein Backend den No-Op aus `IRenderer.h:364`.
- **A2 · Prepass.** GL hat ihn im Wesentlichen: `m_giGBufPosTex`/`NormTex`/
  `MatTex`/`Depth` (`:6489-6518`). Zu klären ist nur, ob GB1-Format und
  Auflösung passen (der GI-Prepass läuft auf `m_giShadowW/H`, nicht auf halber
  Bildschirmauflösung) und ob er auch dann läuft, wenn GI aus und SSR an ist.
- **A3 · Ziele + Pipelines.** `m_ssrHistRad[2]`, `m_ssrHistPos[2]`,
  `m_ssrPingTex`, `m_ssrRoughTex` — halbe Auflösung, RGBA16F, an
  `EnsureGBufferTargets`/`Destroy…` gehängt. Programme aus
  `ssrTrace/ssrBlur/ssrRoughMix(Backend::GLSL410)`.
- **A4 · Trace + Blur + RoughMix zeichnen.** `conv = (+1, 2, −1, 0.1)`,
  Radianzquelle `m_hdrColor` (deferred: aktuell; forward: Vorframekopie),
  Punkt-Sampler auf GB1 und die History-Texturen, `m_fsVAO` für die
  bufferlosen Fullscreen-Draws.
- **A5 · Konsument.** `heSSRFwd` auf eine freie Unit (18 ist belegt,
  `:5142-5147`), `lit.ssr[0..2]` füllen (`:10530` sagt heute ausdrücklich, dass
  es null bleibt) und im Built-in-Szenenshader die Kaskade um den SSR-Zweig
  ergänzen — GL hat die Kaskade bereits, es fehlt nur diese eine Stufe.
- **A6 · Der deferred Composite ist auf GL zusätzlich möglich.** Optional, und
  bewusst hinten: er verdoppelt die Fläche, ohne für die drei Zielbackends etwas
  zu klären.

**Prüfmuster:** `HE_DUMP_SSRTEST=1` (Spiegelboden) und `HE_DUMP_SSRTESTWALL=1`
(kamerazugewandte Spiegelwand) existieren und sind auf Metal eingefahren. Die
Wand ist der schärfere Test: an ihr ist der alte Facing-Gate gescheitert
(`docs/ssr-plan.md` P4).

---

## 5. Checkpoint B — Vulkan

Vorbedingungen: Tiefe liegt vor (Decal-Vorpass), HDR liegt vor
(`m_hdrImage`, wenn `m_postFxReady`). Zu bauen:

- **B1** — `SetSSRSettings`-Override.
- **B2** — MRT-Erweiterung von `m_ssaoPosRenderPass` (Ein-Attachment heute) auf
  drei, oder Anschluss an den vorhandenen `m_giGBuf`-Vorpass. Nach §3.2 Weg (a).
  **Das ist der Aufwand der Phase.**
- **B3** — Trace/Blur/RoughMix-Pipelines gegen `Backend::SpirV`, UBO-Ring wie
  beim Decal-Pass (`SSRTraceUniforms` ist 336 B, also **512-B-Slot-Stride**,
  weil `minUniformBufferOffsetAlignment` nicht mehr als 256 verlangen darf).
- **B4** — `conv = (+1, 1, 0, 0.1)`.
- **B5** — Konsument: `heLight.ssr` füllen **und** die Kaskade in `scene.frag`
  ergänzen (§3.3).

**Grenze, die sich vererbt:** Vulkans Decal-Vorpass zeichnet nur opake statische
Meshes. Wird die Tiefe daher genommen, spiegeln sich Skinned Meshes, Partikel
und WPO-Geometrie nicht. Ein MRT-Prepass nach §3.2 (a) hat dieselbe Grenze —
er ist derselbe Geometriedurchlauf. Melden, nicht verstecken.

---

## 6. Checkpoint C — D3D11

Vorbedingungen: Tiefe sampelbar seit dem Decal-Port (`R24G8_TYPELESS`, DSV+SRV),
HDR **nur im Viewport-Pfad** (§2.2).

- **C1** — `SetSSRSettings`-Override.
- **C2** — MRT-Erweiterung von `createSSAOTargets` (`:1443`, heute ein
  Positions-RT).
- **C3** — Pipelines gegen `Backend::HLSL`, **mit `compileHlslPinned`**. Die
  Bindings der drei Shader liegen jenseits von D3D11s Grenzen und müssen nach
  dem Decal-Vorbild (`kDecalHlslPins`, `:1849`) in den bedienbaren Bereich
  gepinnt werden:

  | Shader | GLSL-Binding | Ressource |
  |---|---|---|
  | `kSSRTraceFS` | 23 (UBO), 19, 20, 22, 24, 25 | Farbe, GB1, Tiefe, 2× History |
  | `kSSRBlurFS` | 23 (UBO), 19 | Eingang |
  | `kSSRRoughMixFS` | 23 (UBO), 19, 20, 21 | scharf, breit, Attribute |

  Das UBO nach `b`, die Texturen nach `t`, die Sampler nach `s` **unterhalb von
  16** — und D3D12 erbt dieselben Zahlen, damit die beiden D3D-Backends einen
  Vertrag teilen statt zweier (genau wie bei den Decals).
- **C4** — `conv = (−1, 1, 0, 0.1)`.
- **C5** — Konsument: die Kaskade in `kSceneHLSL` (§3.3). Für Graph-Materialien
  zusätzlich §2.3 Kopf 1 — `s31` ist unerreichbar, das braucht eine gepinnte
  Preamble und ist ein **eigener, größerer Vorgang**, der nicht stillschweigend
  in diese Phase rutschen darf.
- **C6** — Swapchain-Pfad: entweder ausdrücklich als „SSR nur im Editor-Viewport"
  melden oder ein HDR-Ziel dort nachziehen. **Vorlagepflichtig**, weil es den
  Frame-Aufbau des gepackten Spiels ändert.

---

## 7. Checkpoint D — D3D12

Nach dem Decal-Muster derselbe Weg wie D3D11, nicht der von Vulkan: Tiefe ist
seit dem Decal-Port `R32_TYPELESS` mit DSV und SRV, es gibt keine
Render-Pass-Objekte, der DSV zu lösen ist ein Aufruf. Zusätzlich zu C1–C5:

- **D1** — eigene Root-Signature pro Pass (Root-CBV + Deskriptor-Tabellen +
  statische Sampler), Registernummern **aus C3 geerbt**.
- **D2** — Deskriptor-Slots für die Reflexionstexturen hinten an
  `sceneSrvHeap` anhängen, nicht in die statische Region (`k_decal*Slot` ist das
  Vorbild).
- **D3** — LDR/HDR-PSO-Paar wie bei Sky, Debug-Linien und Decals.
- **D4** — nach dem Pass den Szenenzustand vollständig wiederherstellen: eine
  Root-Signature-Umschaltung löscht alle Root-Argumente.

---

## 8. Prüfungen und ihre Grenzen

Diese Maschine kann GL, Vulkan und D3D **nicht** laufen lassen (kein Display für
GL, kein Vulkan/D3D überhaupt). Die Gates sind dieselben wie beim Decal- und
GI-Port:

1. **Cross-Compile-ctest — die erste Lieferung von Schritt 2.** `ssrTrace`,
   `ssrComposite`, `ssrBlur`, `ssrRoughMix` für `Metal`, `GLSL410`, `HLSL`,
   `SpirV`, jeweils `.ok` geprüft; Vorbild `tests/test_material_graph.cpp:715`
   (der Decal-Fall). Nach §2.4 gab es **keinen einzigen SSR-Test** — das ist das
   erste Netz unter dem Feature und das Einzige, was hier wirklich läuft.
   **Steht** (Schritt 2, alle vier Shader × vier Backends grün). Sobald C3 steht,
   kommt ein zweiter Fall dazu: **die HLSL-Register müssen im bedienbaren
   Bereich von D3D11 liegen** (dasselbe Muster wie der Decal-Registertest).
2. **Offline-Shaderprüfung.** `glslangValidator` ohne `-G` für Desktop-GLSL,
   `xcrun metal` für MSL.
3. **Metal bleibt die Referenz und ist hier real prüfbar.**
   `scripts/he_shot.py` mit `HE_DUMP_SSRTEST=1` / `HE_DUMP_SSRTESTWALL=1` /
   `HE_DUMP_SSRQUALITY` liefert den Vergleichsshot, gegen den jedes portierte
   Backend später gehalten wird. Wird ein Shader geändert (§3.2 Weg (a) hebt den
   Prepass-Shader in die Library), ist ein Metal-A/B **Pflicht**, bevor die
   Änderung blind nach GL/VK/D3D geht — genau die Reihenfolge-Warnung, deren
   Missachtung im Decal-Port dazu geführt hat, dass der Sampled-Pfad bis heute
   nirgends rasterisiert wurde.

   > **Das Rezept, aus Schritt 3, damit es niemand zweimal herleiten muss.**
   > `HE_DUMP_SSRTEST` behauptet in seinem Kommentar den deferred Tile-Pfad —
   > der Prepass läuft aber **nur forward** (`m_fwdReflPrepassWanted =
   > !deferredActive && …`). Ohne `HE_DUMP_RENDERPATH=0` ist ein Prepass-A/B
   > also leer. Der Aufruf, der die Spiegelung zeigt:
   > `python3 scripts/he_shot.py OUT.png SSRTEST=1 SSR=1 RENDERPATH=0 TOD=0.5
   > PITCH=-8 CAMY=2.5 CAMZ=2 COVERAGE=0.2` (Wand: zusätzlich `SSRTESTWALL=1`,
   > `PITCH=-4 CAMY=3`).
   >
   > Und: **der Dump ist nicht bitgenau wiederholbar.** Die Wolken laufen auf
   > der Uhr, der Himmelsstreifen (oben ~270 px) driftet zwischen zwei Läufen
   > desselben Binaries um bis zu ~30/255. Der A/B wird deshalb **bandweise**
   > gelesen — der Geometriestreifen darunter ist bei einer richtigen Änderung
   > pixelgleich — und braucht einen Kontrollshot mit demselben Binary als
   > Rauschmaß.
4. **Vulkan:** `clang++ -fsyntax-only` gegen die von SDL mitgelieferten
   Vulkan-Header. **D3D11/D3D12: gar nichts** — auf diesem Mac gibt es weder
   Header noch `fxc`/`dxc`, die beiden Übersetzungseinheiten werden hier nicht
   gebaut. Der erste Windows-Build ist die erste Prüfung.
5. **Nutzer-Verify auf echter Hardware** wird pro Backend als **offen** gemeldet,
   nicht als erledigt.

`ctest` bleibt in jedem Schritt grün. Schritt 1 ändert keinen Code, es gibt hier
also nichts zu brechen.

---

## 9. Vorgeschlagene Schnittfolge

| # | Inhalt | Hier verifizierbar |
|---|---|---|
| 1 | (dieser Schritt) Kartierung + Plan | — |
| 2 | Cross-Compile-ctest über alle vier SSR-Shader × vier Backends — **erledigt** | ja |
| 3 | §3.2 (a): Prepass-Shader nach kanonischem GLSL in die geteilte Library, Metal darauf umstellen — **erledigt** | ja, visuell (A/B gegen den Referenzshot) |
| 4 | Checkpoint A — OpenGL (Forward-SSR, Kaskade im Szenenshader) | nur offline + ctest |
| 5 | Checkpoint B — Vulkan | nur Syntaxprüfung |
| 6 | Checkpoint C — D3D11 (inkl. HLSL-Register-Pins + Registertest) | nur ctest auf dem HLSL-Text |
| 7 | Checkpoint D — D3D12 (Register aus 6 geerbt) | dito |
| — | §2.3 Kopf 1: gepinnte Preamble für D3D11-Graph-Materialien | eigener Vorgang |
| — | §2.2: HDR im D3D11-Swapchain-Pfad | vorlagepflichtig |
| — | GL-Deferred-Composite (A6) | optional |

Schritte 2 und 3 gehören zusammen und in eine Hand: Schritt 3 ist die **einzige**
Stelle, an der sich eine Shader-Änderung auf dieser Maschine gegen echte Hardware
beweisen lässt, bevor sie blind in vier Backends geht.
