# Deferred Renderer — Umsetzungsplan (als wählbarer Render-Pfad, Metal zuerst)

> Stand: 2026-07-31 · Ziel: Neben dem bestehenden Forward-Renderer ein **Deferred**-Pfad, der
> im Editor und im Spiel umschaltbar ist (`Renderer ▸ Render Path: Forward | Deferred`).
> Metal zuerst (wie GI/SSR), Architektur so, dass die Shading-Mathematik **nicht** ein siebtes
> Mal kopiert wird.

> **Port auf D3D11 / D3D12 / Vulkan (Thema 150, ab 2026-10-05):** Bestandsaufnahme, Pass×Backend-
> Tabelle und Bauplan in **§10**.

> **UMSETZUNGSSTAND 2026-07-31: P0–P7 implementiert (Metal + OpenGL; P6/P7 Metal-only).**
> - P0–P3: `RenderPath`-Enum, Editor-Combo + `config.json`-Persistenz + Game-Read;
>   G-Buffer-Codegen (`MatShaderGen::glslGBuffer`, gemeinsamer Body + zweiter Emit-Tail),
>   `MaterialShaderLibrary::deferredResolve*` aus der geteilten `kLightingPreamble` (heLitP),
>   G-Buffer-Pass + Resolve + Forward-Zusatzpass in Metal **und** GL.
> - P4: A/B via `HE_DUMP_RENDERPATH` (he_shot): mittl. Abweichung ~0.02–0.03/255 ✅.
> - P5: SSAO rekonstruiert View-Positionen aus der G-Buffer-Tiefe (Fullscreen statt
>   Re-Rasterisierung) — Metal-Two-Pass + GL. (Im Tile-Modus bleibt der klassische
>   Prepass: der Resolve konsumiert AO innerhalb von Pass 1.)
> - P6 (Metal/Apple Silicon): Single-Pass, G-Buffer **memoryless** im Tile-Speicher,
>   Resolve per Framebuffer-Fetch (`[[color(n)]]`; 4. Attachment R32F trägt die NDC-Tiefe);
>   Fallback = Two-Pass (Intel, `HE_DEFERRED_TILE=0`).
> - P7 (Metal): **Clustered Lighting** — Punkt/Spot aus per-Cluster-Listen (16×9×24-Grid,
>   CPU-Scatter, bis 256 Lichter), heLight-Fenster nur noch Directional; 8-Licht-Limit
>   gefallen. `HE_DEFERRED_CLUSTER=0` = 8-Licht-A/B-Guard. GL bleibt 8-Licht (kein SSBO in 4.1).
> - Export: `customShaderGBufGlsl` wird als MTRL-Tail-Feld serialisiert und vom Packer
>   byte-verbatim mitgenommen — Packaged Builds rendern Deferred ohne Node-Graph.
> - **Nachträge (ebenfalls umgesetzt):** GI-Local-Ray-Masken für Cluster-Lichter (344dfb2);
>   **SSR v1** nach ssr-plan §4.5 — lag-freier Trace + additiver Composite im Tile-Pfad,
>   Resolve überspringt ambSpec via `heLight.ssr.w` (8341c4d); **Decals v1** — DecalComponent
>   + Projektor-Pass im Tile-G-Buffer via Framebuffer-Fetch (9121baf).
> - **SSR-Blur (ssr-plan P4, v1):** separierbarer 5-Tap-Gauss (confidence-gewichtet) auf dem
>   Half-Res-Trace vor dem Composite, ab Quality Med; Quality Low = roher Trace.
> - Offen: SSR temporale Glättung (ssr-plan P4 v2), SSR/Decals im Two-Pass-Fallback & GL,
>   Profiler-Messung P6 auf echter HW, Reflection-Probes (Off-Screen-Fallback).
>
> **UMGESETZT (2026-07-31): P0–P4 für Metal UND OpenGL.** RenderPath-Enum + Editor-Combo +
> config.json ("RenderPath") + GameApplication-Read; `MatShaderGen::glslGBuffer` (zweiter
> Emit-Tail, gleiche Ausdrucksvariablen, in-memory `MaterialAsset::customShaderGBufGlsl`, bei
> Load/Edit regeneriert — NICHT serialisiert, gepackte Builds ohne Graph routen forward);
> `MaterialShaderLibrary::deferredResolve()/fullscreenVertex()/resolveGBufferShaders()`;
> Metal: `EncodeGBuffer` + Depth-Blit + Resolve im HDR-Pass (`MetalDeferredFrame`-Hand-off);
> GL: MRT-FBO (SRGB8+2×RGBA16F+Depth-Textur, `GL_FRAMEBUFFER_SRGB` im G-Buffer-Pass), Resolve
> mit eigenem `m_resolveLightUBO` (inkl. CSM-Matrizen — die Material-Programme aliasen heCsm
> auf die Local-Atlas-Unit und behalten csmSplits.w=0). Skinned Meshes laufen v1 forward im
> Lighting-Pass (nicht im G-Buffer). Debug: `HE_DUMP_RENDERPATH=1`, `HE_DUMP_GBUFFER=1..4`,
> `HE_RENDER_PATH` (Renderer-Init). P4-Gate headless verifiziert (Metal, MATERIALTEST,
> HE_SKY_TIME gepinnt): mittlere Abweichung **0.021/255**, Ausreißer ≤10/255 auf 0,001 % der
> Pixel (Silhouetten). GL blind (Sandbox ohne Display), Windows/Linux-HW-Verify offen.
> Offen: P5 (SSAO aus dem G-Buffer), P6 (memoryless/Tile-Subpass), P7 (Clustered Lighting),
> G-Buffer-Varianten im Export-Baking (CHUNK_PSHD).

---

## 0. Warum Deferred — und was es kostet

**Gewinn**

| Punkt | Heute (Forward) | Mit Deferred |
|---|---|---|
| Lichtanzahl | hart auf **8** begrenzt (`kMaxLightWindow`, `LightPacking.h:20`) — jede Fläche iteriert alle 8 | Tiled/Clustered → praktisch unbegrenzt, Kosten pro Licht nur dort, wo es leuchtet |
| Overdraw | jedes verdeckte Fragment zahlt die volle PBR-Beleuchtung inkl. CSM-PCF, DDGI-Trilinear, Local-Atlas | Beleuchtung genau **einmal pro sichtbarem Pixel** |
| SSR | braucht Vorframe-Farbe → 1 Frame Lag | G-Buffer liefert `specColor`/`roughness`/Normale → **lag-frei**, siehe `ssr-plan.md` |
| SSAO | eigener Re-Rasterisierungs-Prepass (`EncodeSSAO`, eigenes extract/cull/sort) | liest den G-Buffer → der ganze Prepass entfällt |
| Decals, Light-Shafts, komplexe Post-FX | nicht möglich / teuer | fallen als Nebenprodukt ab |
| Metal-Sampler-Budget | Material-Pipeline **am 16er-Limit** (`MaterialShaderLibrary.cpp:759`) | G-Buffer-Variante braucht nur Graph-Texturen → Problem verschwindet |

**Kosten**

- **Transparenz** kann Deferred nicht → braucht weiterhin einen Forward-Pass (existiert bereits als
  Transparenz-Pass mit eigenen Blend-Pipelines).
- **Bandbreite**: G-Buffer schreiben + lesen. Auf Apple Silicon (TBDR) mit *memoryless*
  Attachments und Single-Pass-Resolve praktisch gratis — auf Desktop-GPUs real (siehe 7.).
- **Kein MSAA.** Die Engine nutzt FXAA (`postfx_fxaa.frag`) — kein Verlust.
- **Zwei Codepfade** in Test- und Verifikationsmatrix.

---

## 1. Ausgangslage

Der Metal-Scene-Pass ist ein einziger Render-Pass auf `m_hdrColor` (RGBA16F, `:6482`), in dem
alles beleuchtet wird: `fragmentMain` (Built-in PBR, `:513-643`) bzw. die generierten
Material-Fragments über `heLitP` (`MaterialShaderLibrary.cpp:437`). Reihenfolge im Frame
(`:8330-8420 ff.`): Shadow → GI-Accel → GI-Shadow → GI-Probes → SSAO → **Scene** (opak → skinned →
Sky → Transparenz) → PostFX.

Material-Pipelines werden per `GetOrBuildMaterialPipeline(shKey, frag, vert, pre, blend)` gebaut
und nach `shKey` gecacht — es gibt also **bereits einen Varianten-Mechanismus** (`blend`), an den
sich eine `gbuffer`-Variante nahtlos anhängt.

---

## 2. Der eine echte Knackpunkt: Materialien müssen Attribute schreiben statt Farbe

Ein generiertes Material endet heute mit (`MaterialGraph.cpp:1081`):

```glsl
oColor = vec4(heApplyFog(heLitP(base, heN, met, rough, vWorldPos, spec, ao) + emis, vWorldPos), opacity);
```

Für Deferred braucht dasselbe Material einen **zweiten Emit-Tail**, der dieselben Ausdrücke in den
G-Buffer schreibt statt sie zu beleuchten. Die gute Nachricht: der Codegen berechnet
`base / met / spec / rough / emis / ao / normalExpr / opacity` bereits als **getrennte Ausdrücke**
(`MaterialGraph.cpp:981-999`) — die G-Buffer-Variante ist ein anderer Schwanz an derselben
Graph-Auswertung, kein zweiter Codegen:

```glsl
// MatShaderGen::gbufferGlsl
oGB0 = vec4(base, met);
oGB1 = vec4(heOctEncode(heN) * 0.5 + 0.5, rough, spec);
oGB2 = vec4(emis, ao);
```

Unlit-Materialien (`Output.p[0] < 0.5`) schreiben `base+emis` nach `oGB2` und `oGB0.rgb = 0` —
der Resolve addiert Emissive ungefiltert, damit bleibt „unlit" auch im Deferred unlit.
Masked (`discard`) funktioniert unverändert. Translucent wird gar nicht erst in den G-Buffer
geroutet (siehe 4.3).

---

## 3. G-Buffer-Layout (v1)

| Target | Format | Inhalt | Bytes/px |
|---|---|---|---|
| GB0 | `RGBA8Unorm_sRGB` | `rgb` = BaseColor, `a` = Metallic | 4 |
| GB1 | `RGBA16Float` | `rg` = Normale (oktaedrisch, `heOctEncode` — existiert schon), `b` = Roughness, `a` = Specular | 8 |
| GB2 | `RGBA16Float` | `rgb` = Emissive (HDR, für Bloom), `a` = Material-AO | 8 |
| Depth | `kDepthFormat` | vorhanden; Weltposition wird per `inverse(viewProj)` rekonstruiert | 4 |

**24 Bytes/Pixel.** Kein Positions-Target (Rekonstruktion aus Depth), kein separates
Motion-Vector-Target in v1.

Bewusst **nicht** im G-Buffer: Wetter (Wetness/Snow) und Fog. Beide sind reine Funktionen von
Normale + Uniforms und werden im Resolve genauso berechnet wie heute in `heLitP` — dadurch bleibt
das Ergebnis per Konstruktion identisch.

---

## 4. Pass-Struktur

```
Shadow ▸ GI-Accel ▸ GI-Shadow ▸ GI-Probes
   ▸ [D] G-Buffer-Pass        (opak + masked + skinned + Landscape + Instancing)
   ▸ [D] SSAO aus dem G-Buffer (statt eigenem Prepass)
   ▸ [D] Lighting-Resolve     (Fullscreen → m_hdrColor)
   ▸ Sky/Clouds               (unverändert, füllt was der G-Buffer nicht deckt)
   ▸ Forward-Zusatzpass       (Transparenz + Partikel + Debug-Linien + Gizmo)
   ▸ PostFX                   (unverändert)
```

### 4.1 G-Buffer-Pass
Derselbe Draw-Loop wie heute (`m_renderGraph` / `GeometryPass` → `DrawCall`s), nur mit
G-Buffer-Pipelines statt Scene-Pipelines. Alle Per-Draw-Binds bleiben: Graph-Texturen 1–4,
Landscape-Weightmap, `HeParams`. **Nicht** gebunden werden: CSM, Local-Atlas, SkyEnv, AO, GI —
die braucht nur der Resolve.

### 4.2 Lighting-Resolve — **eine** Shading-Quelle, keine siebte Kopie
Der Resolve-Fragment-Shader wird aus **derselben** `kLightingPreamble` gebaut wie die
Material-Shader (neue Funktion `MaterialShaderLibrary::deferredResolve(Backend)`):

```glsl
// Fullscreen; alle Preamble-Texturen (CSM, Local-Atlas, SkyEnv, AO, GI, später SSR) gehören
// hier ihm allein — kein Sampler-Budget-Problem.
vec4 g0 = texture(heGB0, uv); vec4 g1 = texture(heGB1, uv); vec4 g2 = texture(heGB2, uv);
float d = texture(heGBDepth, uv).r;
if (g1.b == 0.0 && g0.a == 0.0 && d >= 1.0) discard;      // Hintergrund → Sky-Pass
vec3 P = heReconstructWorldPos(uv, d);                    // inverse(viewProj)
vec3 N = heOctDecode(g1.rg * 2.0 - 1.0);
oColor = vec4(heApplyFog(heLitP(g0.rgb, N, g0.a, g1.b, P, g1.a, g2.a) + g2.rgb, P), 1.0);
```

Damit ist Deferred **per Konstruktion** identisch zu Forward: dieselbe Funktion, dieselben
Uniforms, dieselben Shadow-/GI-/Weather-/Fog-Zweige. Driftet die Preamble, driften beide Pfade
gemeinsam. Das ist der entscheidende Architektur-Punkt dieses Plans.

### 4.3 Forward-Zusatzpass
Translucent-Materialien, Partikel, Debug-Linien und Gizmos laufen weiter durch die heutigen
Forward-Pipelines gegen `m_hdrColor` + G-Buffer-Depth (read-only). Der Transparenz-Pass
(`MetalRenderer.mm:8042 ff.`) wird dafür fast unverändert wiederverwendet — inklusive seiner
bereits existierenden Blend-Varianten der Material-Pipelines.

Routing-Regel im Draw-Loop: `blendMode == Translucent` → Forward-Zusatzpass, sonst G-Buffer.

---

## 5. Auswahl-Mechanik

```cpp
// Types/Enums.h
enum class RenderPath : uint8_t { Forward = 0, Deferred = 1 };

// IRenderer.h
virtual void SetRenderPath(RenderPath) {}
virtual RenderPath GetRenderPath() const { return RenderPath::Forward; }
bool supportsDeferred = false;   // Capabilities — v1: nur Metal
```

- **Editor:** Combo direkt unter der RHI-Auswahl (`EditorSettingsPanel.cpp:103`), ausgegraut wenn
  `!supportsDeferred`; Persistenz als `RenderPath` in `config.json` (Muster:
  `EditorApplication.cpp:669/1175/2945`).
- **Spiel:** `GameApplication` liest denselben GlobalState-Key (wie GI/GpuParticles).
- **Umschalten zur Laufzeit** ist möglich, ohne den Pipeline-Cache zu leeren: die
  G-Buffer-Varianten bekommen einen eigenen Cache-Key (wie `blend` heute). Erster Frame nach dem
  Umschalten baut die fehlenden PSOs (kurzer Hitch) — akzeptabel, weil es eine Editor-Aktion ist.
  `WarmupMaterials` wird um die G-Buffer-Variante erweitert.

---

## 6. Phasenplan

### P0 — Infrastruktur, ohne Wirkung
`RenderPath`-Enum, `SetRenderPath`/`GetRenderPath`, `Capabilities::supportsDeferred`, Editor-Combo
+ Persistenz + Game-Read. Deferred wählbar, rendert aber noch Forward.
**Verifikation:** Tests grün, `he_shot`-A/B pixel-identisch.

### P1 — G-Buffer schreiben
`MatShaderGen::gbufferGlsl` (zweiter Emit-Tail im Codegen), `kGBufferMSL` für den Built-in-Pfad,
`EnsureGBufferTargets`, G-Buffer-Varianten in `GetOrBuildMaterialPipeline`, Draw-Loop-Routing.
Debug-Ausgabe `HE_DUMP_GBUFFER=0..3` zeigt ein Target direkt im Backbuffer.
**Verifikation:** headless — BaseColor/Normalen/Roughness-Views plausibel, Graph-Materialien
(inkl. Landscape-Layer und WPO) landen korrekt im G-Buffer.

### P2 — Resolve
`MaterialShaderLibrary::deferredResolve(Backend)` + `EncodeDeferredResolve`. Noch ohne Sky und
Transparenz.
**Verifikation:** opake Szene sieht aus wie Forward.

### P3 — Vollständigkeit
Sky/Clouds, Forward-Zusatzpass (Transparenz, Partikel, Debug, Gizmo, Selection-Outline), Skinned
Meshes, Instancing, Masked.
**Verifikation:** die bestehenden Witness-Szenen (`HE_DUMP_LOCALSHADOW`, `MATERIALTEST`,
`LANDSCAPELAYERS`, `GIBLEED`) laufen in **beiden** Pfaden.

### P4 — Paritäts-Gate
Neues Skript `scripts/he_path_ab.py`: rendert jede Witness-Szene in Forward und Deferred und
vergleicht. Zielkriterium: **mittlere Abweichung < 1/255**, Ausreißer nur an
Silhouetten (Normalen-Quantisierung). Erst wenn das steht, gilt Deferred als benutzbar.

### P5 — Erste Ernte
SSAO liest den G-Buffer statt eigenem Prepass (streicht ein komplettes extract/cull/sort +
Re-Rasterisierung). DDGI/GI-Masken im Resolve verifizieren.
**Verifikation:** Profiler-Capture Forward vs. Deferred, gleiche Szene.

### P6 — Apple-Silicon-Optimierung
G-Buffer-Attachments auf `MTLStorageModeMemoryless`, Resolve als **zweiter Subpass im selben
Render-Pass** (Tile-Shading, `[[color(n)]]`-Eingänge). Damit verlässt der G-Buffer nie den
Tile-Speicher → Bandbreitenkosten ≈ 0.
**Verifikation:** Profiler — G-Buffer-Pass-Zeit muss messbar fallen; Bild identisch zu P4.

### P7 — Der eigentliche Gewinn: Clustered Lighting
Light-Culling in ein 3D-Cluster-Grid (Compute), Resolve iteriert nur die Lichter seines Clusters.
Erst hier fällt das 8-Licht-Limit — bis dahin ist Deferred bewusst *funktional identisch* zu
Forward, damit P4 überhaupt greifen kann. Danach: Decals als Folgefeature.

---

## 7. Bandbreiten- und Perf-Budget

Bei 2560×1440: 24 Byte/px × 3.7 Mpx ≈ **88 MB** G-Buffer-Write + ~88 MB Read im Resolve.

- **Apple Silicon nach P6:** memoryless → kein DRAM-Traffic, der G-Buffer lebt im Tile-Speicher.
  Erwartung: Deferred ist ab mittlerer Szenenkomplexität **schneller** als Forward, weil Overdraw
  nicht mehr beleuchtet wird und der SSAO-Prepass entfällt.
- **Vor P6 / Desktop-GPUs:** ~176 MB/Frame zusätzlicher Traffic — bei 60 fps ~10 GB/s. Für
  Desktop-GPUs unkritisch, auf Intel-Macs mit iGPU spürbar → dort Forward als Default lassen.

Ziel: Deferred darf in der Referenzszene **nicht langsamer** sein als Forward, sonst ist P6
Voraussetzung für die Freigabe.

---

## 8. Risiken

| Risiko | Gegenmaßnahme |
|---|---|
| Zwei Shading-Pfade driften auseinander | Resolve wird aus **derselben** `kLightingPreamble` gebaut und ruft `heLitP` — es gibt keine zweite Implementierung. P4 ist das automatisierte Gate. |
| Codegen-Variante vergisst ein Attribut (z. B. Specular) | Der G-Buffer-Tail nutzt exakt dieselben Ausdrucks-Variablen wie der Forward-Tail; Unit-Test vergleicht, dass beide Varianten aus demselben Graph dieselbe Ausdrucksmenge referenzieren |
| Transparenz-Sortierung/Look ändert sich | Der Transparenz-Pass wird unverändert übernommen, nur sein Depth-Input wechselt auf den G-Buffer-Depth |
| Normalen-Quantisierung (oct in RG16F) sichtbar | 16 Bit oct ≈ 0.004° Fehler — unkritisch; Formatwechsel wäre ein Einzeiler |
| Emissive-HDR verliert Bloom-Kopf | GB2 ist RGBA16F, kein 8-Bit |
| Umschalten mitten im Frame / halbe Pipelines | `SetRenderPath` wirkt erst zum nächsten Frame; `WarmupMaterials` baut beide Varianten |
| Materialien, die im Forward Sonderwege gehen (Unlit, Custom-Escape-Hatch-GLSL mit eigenem `heLit`) | Unlit über GB2-Emissive; hand geschriebene Fragments ohne Graph → automatisch in den Forward-Zusatzpass routen (sie haben keinen G-Buffer-Tail) |

---

## 9. Wechselwirkung mit SSR

`docs/ssr-plan.md` ist auf diesen Plan abgestimmt: SSR bekommt **einen** Trace-Pass und **zwei**
Composite-Wege — im Forward über `heSSR` in `heLitP` (Vorframe-Farbe, 1 Frame Lag), im Deferred
als eigener Reflexions-Pass nach dem Resolve mit den exakten G-Buffer-Daten (**kein** Lag).
Reihenfolge-Empfehlung: **P0–P2 dieses Plans zuerst**, dann SSR — dann muss der SSR-Composite nur
einmal geschrieben werden statt zweimal.

---

## 10. Port auf D3D11, D3D12 und Vulkan — Bestandsaufnahme und Bauplan (Thema 150, Schritt 1)

> Stand: 2026-10-05 · Zweig `claude/deferred-renderer-fuer-d3d11-d3d12-und-vulkan-volle-paritaet`
> = main `469fa9f6`. Alle Zeilennummern beziehen sich darauf. Nur Bestandsaufnahme, kein
> Renderer-Code, kein bestehender Pfad geändert.
>
> Methode: vier getrennte Inventuren (Metal/GL-Referenz, D3D11, D3D12, Vulkan) mit
> `Datei:Zeile`, danach jede Aussage, auf der der Bauplan steht, von Hand am Code nachgeprüft.
> Was nur gelesen und nicht gemessen ist, ist so markiert. Echte Hardware hat keiner der drei
> Ziel-Backends hier gesehen, und das bleibt bis zum Ende des Themas so.

Abkürzungen: **D11** = `src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp`, **D12** =
`…/D3D12/D3D12Renderer.cpp`, **VK** = `…/Vulkan/VulkanRenderer.cpp`, **MTL** =
`…/Metal/MetalRenderer.mm`, **GL** = `…/OpenGL/OpenGLRenderer.cpp`, **MSL** =
`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`, **MB** =
`src/HE_Rendering/include/Backends/D3D11/D3D11MaterialBindings.h`, **MRS** =
`…/include/Backends/D3D12/D3D12MaterialRootSignature.h`, **VML** =
`…/include/Backends/Vulkan/VulkanMaterialLayout.h`, **VAL** =
`scripts/validate_embedded_shaders.py`, **TMG** = `tests/test_material_graph.cpp`.

### 10.1 Was sich seit der Analyse aus Thema 116 geändert hat

Die erste Port-Analyse (`docs/deferred-d3d-vulkan-analysis-2026-10-01.md`) liegt nur auf dem nie
gemergten Zweig `origin/claude/deferred-rendering-d3d-vulkan`. Sie hat den Port damals
zurückgestellt und drei Dinge vorgezogen. Alle drei sind inzwischen auf main:

| Vorbedingung aus 116 §7 | Stand heute | Beleg |
|---|---|---|
| Clustered für Graph-Materialien (`heLitP`) | erledigt (Thema 117): `fragmentClustered`; D3D11 Raw-Buffer t24–t26, D3D12 Root-SRVs, Vulkan Bindings 24–26 | MSL:2668–2673; D11:1296–1333; D12:10206; VK:2522 |
| HDR/PostFX im Spielpfad | erledigt (Thema 130, PR #89): der Spielzweig fährt den Viewport-Frame in Backbuffer-Größe und kopiert ihn | D11:7121–7176; D12:10754–10862; VK:423–467 |
| Laufzeit-Zeuge für Vulkan | erledigt (PR #90): CI-Job `vulkan-lavapipe` + `scripts/he_vk_imagetests.py` | `.github/workflows/ci.yml:387–505` |

Drei Aussagen aus 116 bzw. `backend-parity-plan.md` gelten so nicht mehr:

- **Tiefen-Übergabe per Kopie** (116 §4.2c: „D3D11 `CopyResource`, D3D12 Barrier + Copy“) ist
  auf D3D nicht nötig. Seit dem Decal-Port liegt die Szenentiefe auf beiden D3D-Backends
  typeless mit DSV **und** SRV vor (D11:3976–3990, D12:4248–4274), und der Decal-Pass ist das
  fertige Muster: DSV vom Output-Merger nehmen, Tiefe als Textur lesen, zurückhängen
  (D11:4791–4874, D12:8937–9002). Der G-Buffer-Pass schreibt direkt in diese Tiefe, der
  Forward-Schwanz testet danach dagegen. Auf Vulkan gilt das nicht (10.5).
- **`backend-parity-plan.md` §1.4 „die Sperre ist eine Zeile“:** für SPIR-V annähernd richtig,
  für HLSL nicht, dort fehlen die Register-Pins (10.4). Die Zeilenangaben dort (`:1024`,
  `:1030`) sind veraltet, heute MSL:1324–1336.
- **„Vulkan ohne Laufzeit-Zeugen“** (116 §4.2, §5) ist mit PR #90 überholt.

### 10.2 Referenz: was Metal und GL im Deferred-Pfad tatsächlich tun

Vorbild für D3D11/D3D12/Vulkan ist **nicht** der Metal-Tile-Pfad (P6, Framebuffer-Fetch,
Apple-only), sondern der **Metal-Two-Pass-Fallback** bzw. GL: G-Buffer als gespeicherte
Texturen, eigener Resolve-Pass, Forward-Schwanz gegen die G-Buffer-Tiefe.

| | Metal Two-Pass | OpenGL |
|---|---|---|
| Entscheidung pro Frame | `m_renderPath == Deferred && EnsureDeferredPipelines()` MTL:16066–16068 | dasselbe GL:10727–10728 |
| Targets | `EnsureGBufferTargets` MTL:10958–11001: GB0 RGBA8-sRGB, GB1/GB2 RGBA16F, GB3 R32F (NDC-Tiefe), Depth32F | GL:7596–7658: GB0 `SRGB8_ALPHA8`, GB1/GB2 `RGBA16F`, Depth24-Textur; nur 3 Draw-Buffer, `oGB3` fällt weg |
| G-Buffer-Pass | `EncodeGBuffer` MTL:15363–15734; eingebaut `gbufferMain` MTL:942–976 | inline GL:11104–11329; eingebaut `kGBufFS` GL:685–728 |
| Routing | Translucent → Forward, Graph ohne GB-Variante → `forwardOpaque`, Skinned immer Forward (MTL:15586–15618) | dasselbe (`deferredForward` GL:11191–11256), Skinned GL:11889 |
| Resolve | `deferredResolve[Clustered]` MTL:13541–13594, `depthParams = (-1, 1, 0)` | nur `deferredResolve(GLSL410)` GL:7745; eigenes `m_resolveLightUBO` GL:7798–7815; `depthParams = (1, 2, -1)` GL:11772–11780 |
| Tiefe für den Schwanz | Blit `m_gbDepth`→`m_hdrDepth` MTL:16236–16251 | Blit GL:11725–11731 |
| Clustered im Resolve | ja (MTL:14236–14278) | nein (GL 4.1 ohne SSBO) |
| SSAO aus G-Buffer (P5) | ja, halbe Auflösung (MTL:11721) | ja, volle Auflösung (GL:11690–11719) |
| Decals deferred | **nein**, nur im Tile-Pfad (MTL:14378) | ja, `decalFragmentSampled` in GB0 (GL:11594–11679) |
| SSR deferred | **nein**, nur im Tile-Pfad (MTL:16101–16102) | **nein** (GL:10733–10734) |
| GI-Reflexionen deferred | nur im Tile-Pfad (MTL:16107) | ja, im Resolve über `heGIReflFwd` |

Für den Begriff „Parität“ heißt das: den vollen Umfang (Clustered **und** Decals **und** SSR
**und** GI-Reflexionen im Deferred-Pfad) hat heute nur der Metal-Tile-Pfad. Der Metal-Two-Pass
hat davon nur Clustered, GL nur Decals und GI-Reflexionen. Zielzustand für D3D/Vulkan ist
deshalb die Two-Pass-Struktur plus Clustered (wie Metal), Decals in GB0 (wie GL) und SSR aus
dem G-Buffer (wie Metal-Tile, nur gesampelt statt per Fetch). GI-Reflexionen fallen heraus,
weil es sie auf diesen drei Backends auch im Forward-Pfad nicht gibt (10.6).

### 10.3 Pass × Backend

**Übersicht.** `JA` = vorhanden · `fwd` = nur im Forward-Pfad · `--` = fehlt. Die D3D11-Spalte
ist der Stand nach Schritt 2 (10.9), die D3D12-Spalte der nach Schritt 3 (10.10), die
Vulkan-Spalte der nach Schritt 4 (10.11); die Zeilen AO, Decals und SSR sind für alle drei auf dem
Stand nach Schritt 5 (10.12).

| Pass | Metal | GL | D3D11 (S2) | D3D12 (S3) | Vulkan (S4) |
|---|:--:|:--:|:--:|:--:|:--:|
| G-Buffer | JA | JA | JA | JA | JA (mit GB3) |
| Lighting-Resolve | JA | JA | JA | JA | JA |
| Clustered im Resolve | JA | -- | JA | JA | JA |
| CSM + Point/Spot-Atlas | JA | JA | JA (über heLitP im Resolve) | JA (wie D3D11) | JA (wie D3D11) |
| Sky / SkyEnv | JA | JA | JA | JA | JA |
| AO (SSAO/HBAO/GTAO) | JA (aus G-Buffer) | JA (aus G-Buffer) | JA (Prepass-Ergebnis; aus G-Buffer offen, 10.12) | wie D3D11 | wie D3D11 |
| GI (DDGI, Masken) | JA | JA | JA | JA | JA |
| Decals | nur Tile | JA (GB0) | JA (GB0, S5) | JA (GB0, S5) | JA (GB0, eigener Pass, S5) |
| SSR | nur Tile | -- (fwd JA) | JA (Forward-Trace, Composite im Resolve, S5) | wie D3D11 | wie D3D11 |
| AA / TAA | JA | JA | JA | JA | JA |
| Transparenz (Forward-Schwanz) | JA | JA | JA | JA | JA |
| `supportsDeferredRendering` | JA | JA | JA | JA | JA |

**Ressourcen und Bedarf je Pass.** „Ist“ ist der heutige Forward-Pfad, „Bedarf“ das, was der
Deferred-Pfad zusätzlich braucht.

**G-Buffer.** Layout aus §3, auf allen drei Zielen ohne neue Formatunterstützung (RGBA16F-MRTs
legen alle drei schon an):
- D3D11: GB0 `R8G8B8A8_UNORM_SRGB`, GB1/GB2 `R16G16B16A16_FLOAT` (je RTV + SRV), Tiefe ist die
  vorhandene `R24G8_TYPELESS` (DSV `D24_UNORM_S8_UINT`, SRV `R24_UNORM_X8_TYPELESS`,
  D11:3976–3990). Anlegen in `createHDRTargets` (D11:2121–2167). MRT ist erprobt
  (Refl-Prepass mit 3 RT D11:2430–2433, GI-G-Buffer D11:3448). Der Material-Cache ist heute
  nur nach dem Shader-Hash geschlüsselt (D11:1341, :4489) → eigener Schlüsselraum für
  G-Buffer-Varianten.
- D3D12: dieselben Formate; Tiefe `R32_TYPELESS` (DSV `D32_FLOAT`, SRV `R32_FLOAT`,
  D12:4248–4274, Viewport D12:1679–1705). Anlegen in `createPostFXResources`
  (D12:1917–2016), jedes Ziel mit eigenem 1-Slot-RTV-Heap wie heute (D12:1930–1960). Der
  PSO-Schlüssel `hash ^ hdr ^ transparent` (D12:8509–8511) bekommt die Dimension `gbuffer`
  (`NumRenderTargets = 3`), der FXC-Bytecode-Cache (D12:8513–8629) teilt sich. MRT-PSO
  erprobt (D12:5651–5673). Ressourcenzustände über eigene Member + `barrier12`
  (D12:2241–2252), es gibt keinen Zustands-Tracker.
- Vulkan: GB0 `VK_FORMAT_R8G8B8A8_SRGB`, GB1/GB2 `VK_FORMAT_R16G16B16A16_SFLOAT`, Tiefe D32.
  Neuer `VkRenderPass` + Framebuffer; heute existiert jede Szenen-Pipeline schon zweimal
  (`m_renderPass` und `m_postFxSceneRP`, VK:3962–4044), der G-Buffer-Pass macht daraus einen
  dritten Satz. Material-Schlüssel (VK:2799–2805) um `gbuffer` erweitern. Die Szenentiefe ist
  **nicht sampelbar** (VK:4695, nur `DEPTH_STENCIL_ATTACHMENT_BIT`), siehe 10.5.
- Alle drei: eingebauter G-Buffer-Fragment-Shader (gibt es nur als Metal `gbufferMain` und GL
  `kGBufFS`). Graph-Materialien: `resolveGBufferShaders` (MSL:977–994) + `fragment()`; auf
  HLSL greift damit schon die Material-Pin-Tabelle (MSL:2639–2674). Der G-Buffer-Tail schreibt
  immer auch `oGB3 = gl_FragCoord.z` an Location 3 (`MaterialGraph.cpp:1379`). Ohne gebundenes
  viertes Ziel wird das verworfen (D3D11 still, D3D12/Vulkan evtl. mit Debug-Layer-Warnung,
  in Schritt 3/4 prüfen).

**Lighting-Resolve (inkl. Clustered).**
- D3D11: `deferredResolve[Clustered](HLSL)` mit neuer Pin-Tabelle (10.4); `HeResolve`-CB;
  `depthParams = (-1, 1, 0)`, dieselbe Konvention, die der Decal-Pass schon füllt
  (D11:4838–4840); Lighting-Fill wie für Graph-Materialien (`m_matLightCB` D11:4388,
  `fillMatLight` D11:6034–6146), Cluster-Listen raw t24–t26 (MB:150–177, D11:5943–5951).
- D3D12: Resolve-Root-Signatur = Material-Signatur (MRS:114–214) + vier G-Buffer-SRVs +
  `HeResolve`-CBV. D3D12 verlangt, dass die Signatur **jedes** statisch referenzierte Register
  deckt, sonst `E_INVALIDARG` beim PSO (MSL:2626–2631). Alle Eingänge eines Draws in einem
  CBV_SRV_UAV-Heap (D12:3617–3621); Cluster als Root-SRVs nach jedem Signaturwechsel
  (`bindClusterRoots` D12:3181–3187); `depthParams = (-1, 1, 0)` (D12:8972–8974);
  Tiefe DEPTH_WRITE→PIXEL_SHADER_RESOURCE wie im Decal-Pass.
- Vulkan: `deferredResolve(SpirV)` übersetzt schon (kanonische Bindings, kein Pin nötig), die
  Clustered-Variante ist gesperrt (MSL:1334). Eigenes Set-Layout für 0, 10–13, 15–18,
  **19–23**, 24–26, 31–33; VML hat 19–23 nicht (VML:50–96). `depthParams = (+1, 1, 0)` wie
  der Decal-Pass (VK:3668–3670).

**Shadows (CSM 3 Kaskaden, Point/Spot-Atlas 16 Layer).** Auf allen drei vorhanden: D11:1401,
:1429–1430, :6270–6280; D12:1463, :1508–1550, :9857–9877; VK:5926–5975. Kein neuer Pass; der
Resolve bindet `heCsm`/`heLocalShadow` wie ein Graph-Material (D3D t12/t13, Vulkan 12/13). CSM
gilt für Graph-Materialien heute nur bei GI aus (D11:6101–6119, D12:9647–9665, VK:6880–6894),
das bleibt so. Offen und für beide Pfade gleich: Thema 146 (Vulkan-Kaskaden einen Frame hinter
der Sonne).

**Sky / SkyEnv.** D3D11 und D3D12 zeichnen den Himmel **zuerst**, ohne Tiefentest
(D11:5816–5823, D12:9389–9395). Das passt ohne Umbau: der Resolve verwirft `d >= 1`
(MSL:1268), der Himmel bleibt stehen. Bedingung: zwischen Himmel und Resolve löscht niemand das
HDR-Ziel, und der G-Buffer-Pass bindet es nicht. Vulkan zeichnet den Himmel zwar auch zuerst,
aber **innerhalb** von `m_postFxSceneRP` (VK:6640–6646); dort ist das der eigentliche Umbau
(10.5). SkyEnv-Cube gibt es auf allen drei nur für Graph-Materialien (D11:4409–4469,
D12:8416–8496, VK:2652–2786), der Resolve bindet ihn an 15.

**AO.** Heute ein eigener SSAO-Prepass, der opake Geometrie neu rastert (D11:2394–2583,
D12:6099–6204, VK:11122–11234); Graph-Materialien lesen das Ergebnis als `heAO` (16,
`texelFetch`, Gate `fog.w`). **v1:** der Resolve liest dasselbe Ergebnis. **Danach (P5):** SSAO
aus der G-Buffer-Tiefe (Vorlage GL `kSSAODepthPosFS` GL:2766–2782); dann muss SSAO vor dem
Resolve laufen und der zweite Lighting-Fill mit dem AO-Gate danach (wie GL:11690–11719).

**GI.** SW-DDGI mit eigenem Halb-Res-Welt-G-Buffer auf allen drei (D11:3264–3603, D12 plus DXR
D12:7605–7677, Vulkan plus Ray-Query VK:9929–10279). Die GI-Pässe bleiben unverändert, der
Resolve bindet Masken (10/11) und DDGI-Atlanten (17/18) und bekommt `giParams`. Den
Welt-G-Buffer durch den Deferred-G-Buffer zu ersetzen ist **nicht** Teil des Themas, Metal
tut das auch nicht (MTL:16014–16024). Offen: Thema 145 (Vulkan `InvalidImageLayout` bei GI).

**Decals.** Heute auf allen drei Forward und selbst beleuchtet (ein Richtungslicht + Ambient,
`decalFragmentForward`; D11:4776–4888, D12:8895–9004, VK:3522–3609 plus Tiefen-Vorpass). Im
Deferred-Pfad: `decalFragmentSampled` blendet **vor** dem Resolve in GB0, Box-Clip gegen die
G-Buffer-Tiefe. Auf HLSL ist es schon gepinnt (b13/t14/t15, MSL:2386–2406), `decalBlend` auf
D3D11 ist schon RGB-only auf RT0 (D11:4708–4723). Nur GB0 binden (wie GL `m_gbDecalFBO`);
auf Vulkan ist das Pflicht, weil `independentBlend` nicht aktiviert ist (VK:1251–1271, kein
Treffer). Der Vulkan-Tiefen-Vorpass entfällt im Deferred-Pfad.

**SSR.** Heute auf allen drei Forward: Refl-MRT-Prepass, Trace, Blur, Vorframe-HDR-Kopie
(D11:2425–2957, D12:5587–6079, VK:11314–11469 + :11917–11960), kein Composite. Deferred wie
Metal `EncodeSSRPasses` (MTL:14609–14826): Trace aus GB1 + Tiefe statt Prepass, `ssrComposite`
nach dem Resolve. `ssrComposite` ist für HLSL **ungepinnt** (MSL:1948–1972) und braucht eine
Tabelle wie der Resolve; der Resolve setzt dann `heLight.ssr.w = 1`, damit `heLitP` ambSpec
überspringt.

**AA / TAA.** Jitter, RG16F-Velocity, Resolve, Sharpen auf allen drei (D11:5809–5814,
:1772–1833, :7038–7083; D12:9383–9388, :10474–10532, :2332–2389; VK:679–683, :4546–4596). Bedarf: Velocity
**nach** dem G-Buffer-Pass gegen die dann gefüllte Tiefe (siehe 10.7 Punkt 2), Post-Kette
unverändert. Die Resolve-`invViewProj` kommt aus der gejitterten Matrix, mit der auch gerastert
wurde.

**Transparenz und Forward-Schwanz.** Nach dem Resolve unverändert: Transparenz back-to-front
mit Read-only-Tiefe (D11:6930–6937, D12:10557–10563, VK:7459–7466), dazu der Replay der opaken
Draws ohne G-Buffer-Variante, Skinned (bleibt Forward wie auf Metal/GL), Partikel/Ribbons,
Debug-Linien. Vulkan zeichnet Skinned heute **nach** den Transparenten (VK:7468–7470); im
Schwanz gehört es davor, wie auf Metal/GL. Auf D3D12 braucht der Schwanz weder einen
Read-only-DSV noch den Zustand `DEPTH_READ`: der heutige Frame macht dieselbe Abfolge schon,
`EncodeDecals` sampelt die Tiefe (DEPTH_WRITE→PIXEL_SHADER_RESOURCE, D12:8937–8939), hängt sie
zurück (D12:9000–9002), danach testen die Transparenten dagegen (D12:10556 ff.). Ob ein Draw
Tiefe schreibt, legt der PSO fest (`DepthWriteMask`), nicht der Ressourcenzustand.

**Anbindung.** Keines der drei Backends überschreibt `SetRenderPath`/`SetViewMode` oder setzt
das Flag (D11:7210–7229, D12:10975–10994, VK:986–1012). `m_renderPath` und `m_viewMode` sind im
Interface `protected`, ein Override ist nicht nötig. Flag wie bei SSR/TAA:
`supportsDeferredRendering = postFxReady && Resolve gebaut` — der direkte Swapchain-Zweig ohne
HDR-Ziel (D11:7185–7203) bleibt Forward. Editor und Spiel brauchen nichts, sie hängen nur am
Flag (`EditorApplication.cpp:3038`, `:4791`; `GameApplication.cpp:3264`;
`ViewportToolbar.cpp:591`). G-Buffer-Ansichten: `viewModeGBufferIndex` in `depthParams.w`, wie
Metal und GL.

### 10.4 Gemeinsame Vorarbeit in `MaterialShaderLibrary`

1. **HLSL-Pins für den Resolve.** `compileResolveVariant` übersetzt alles außer Metal ungepinnt
   (MSL:1330–1335). SPIRV-Cross legt `binding = N` dann auf `register(tN/sN/bN)`: G-Buffer
   s19–s22, Preamble s16–s18 und s31–s33, `HeResolve` b23. SM 5.0 endet bei s15 und b13
   (MSL:2578–2590). Gelesen, nicht durch FXC geschickt; die Material-Fragmente hatten bis zu
   ihrer Pin-Tabelle genau diesen Fehler (X4509). Vorschlag, aufbauend auf `kHlslMaterialPins`:

   | GLSL-Binding | Ressource | HLSL |
   |---|---|---|
   | 0 | `HeLighting` | b0 (wie Material) |
   | 10, 11, 12, 13, 15 | GI-Masken, CSM, Local-Atlas, SkyEnv | t/s gleich Binding (wie Material) |
   | 16 | `heAO` (`texelFetch`, Sampler tot) | t16/s0 (wie Material) |
   | 17, 18 | DDGI-Atlanten | t17/s1, t18/s3 (wie Material) |
   | 31, 32, 33 | Forward-SSR, GI-Refl, Cloud-Shadow | t31/s8, t32/s9, t33/s14 (wie Material) |
   | 19, 20, 21, 22 | `heGB0..2`, `heGBDepth` | **t19/s2, t20/s4, t21/s5, t22/s6** (neu: der Resolve deklariert weder `heTex0` noch `heTexP0..3`, deren Sampler sind frei). **Umgesetzt in Schritt 2 als t27..t30** mit denselben Samplern, siehe 10.9 |
   | 23 | `HeResolve` | **b-Register ≤ b13**, Vorschlag b4 (im Resolve-Draw nur neben b0 belegt) — so umgesetzt |
   | 24, 25, 26 | Cluster-Listen | t24–t26 als `ByteAddressBuffer`, kein Sampler (Vertrag aus Thema 117) |

   `heLandscapeWeights` (14) kommt aus dem Graph-Codegen, nicht aus der Preamble, der Resolve
   deklariert ihn nicht. Damit hat der Resolve 15 Sampler-Deklarationen, davon 14 lebend: passt
   in s0–s15, mit s7 als einzigem freien Rest.
2. **Clustered-Sperre öffnen** (MSL:1334) für HLSL und SPIR-V; GLSL410 bleibt gesperrt.
   Bindings wie in Thema 117, die Puffer existieren auf allen drei schon.
3. **Tests.** Heute übersetzt **kein** Test `deferredResolve*` oder `fullscreenVertex` für
   irgendein Backend. Neu: Registertest für die Resolve-HLSL (Muster SSR TMG:1127, Decal
   TMG:1184), FXC-Lauf auf Windows (Muster TMG:3521, :4197), SPIR-V-Reflexion gegen das neue
   Resolve-Set-Layout (Muster TMG:3113–3270). Der Validator (VAL) deckt Library-Shader nicht
   ab, er sieht nur eingebettete Strings.
4. **Eingebauter G-Buffer-Shader.** Er muss die VS-Ausgabe und den Konstantenpuffer des
   jeweiligen eingebauten Szenen-Shaders lesen, und die sind pro Backend eigene Handkopien
   (`kSceneHLSL` in D11 und D12 je eine, Vulkan `shaders/scene.frag`). Empfehlung: ein
   zusätzlicher Pixel-Entry neben der jeweiligen `kSceneHLSL`-Kopie mit eigenem Namen
   (z. B. `GBufPS`, kein `main`, sonst greift die `main`→`ps_5_0`-Heuristik) und
   `shaders/gbuffer.frag` für Vulkan. Ein cross-kompilierter gemeinsamer Shader (Vorschlag aus
   116 §4.1) wäre schöner, bräuchte aber einen gemeinsamen Binding-Vertrag für die drei
   Built-in-Shader, den es nicht gibt.
5. **`ssrComposite` pinnen** (MSL:1948–1972), erst in Schritt 5.

### 10.5 RHI-Bausteine

| Baustein | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| MRT | ja (D11:2430–2433) | ja (D12:5651–5673) | ja (VK:9674–9698, :11784–11807) |
| Szenentiefe als Shader-Eingang | ja, SRV (D11:3976–3990) | ja, SRV (D12:4248–4274) | **nein** (VK:4695); seit Schritt 4 GB3 (R32F) statt der Tiefe, 10.11 |
| Compute | cs_5_0 (D11:3198–3201) | cs_5_0 + DXR cs_6_5 (D12:6784–6837, :7605–7677) | ja (VK:9462–9471) |
| Structured / Raw / SSBO | structured t18–t20, raw t24–t26 (MB:150–177) | Root-SRVs t18–t20, t24–t26 (D12:4596–4609, MRS:156–166) | SSBO 10–12 und 24–26 (VK:1896–1902, VML:46–48) |
| Bindungsdeckung | keine statische Prüfung | Root-Signatur muss alles decken (MSL:2626–2631) | Set-Layout muss alles decken; 14 seit Thema 143 in VML, der Resolve hat seit Schritt 4 eine eigene Tabelle `kResolveBindings` (10.11) |
| Render-Pass-Objekte | keine | keine (Formate im PSO) | ja, Pipeline je Render-Pass (heute zwei Sätze) |
| Blend pro Ziel verschieden | möglich, ungenutzt | möglich, ungenutzt | **nein**, `independentBlend` aus (VK:1251–1271) |
| Fullscreen-VS | `kFSTriangleVS` (`HlslSources.h:386–397`) oder `fullscreenVertex` (MSL:2431–2443, ohne Bindings) | dasselbe | `postfx.vert` oder `fullscreenVertex` (bei SSR schon genutzt, VK:11331) |
| Zustände / Layouts | implizit (OM lösen und neu binden) | Member + `barrier12` (D12:2241–2252) | CPU-Layout-Tracking; `runPostFXBarrier` kann nur Farbe (VK:3733–3743) |
| Laufzeit-Zeuge ohne GPU | WARP nur in he_tests (TMG:4297–4316); der Renderer erzeugt nur HARDWARE-Geräte (D11:5664–5669) | WARP nur in he_tests (TMG:3662–3678) | lavapipe im CI-Job `vulkan-lavapipe` |

**Vulkan-Tiefe, zwei Wege.** (A) Szenentiefe mit `VK_IMAGE_USAGE_SAMPLED_BIT` anlegen und
zwischen G-Buffer-Pass, Resolve und Schwanz die Layouts wechseln. (B) GB3 als viertes
Farbziel `VK_FORMAT_R32_SFLOAT`, wie im Metal-Layout: der G-Buffer-Tail schreibt `oGB3`
ohnehin (`MaterialGraph.cpp:1379`), der Resolve sampelt GB3 als `heGBDepth`, das Tiefenbild
bleibt reines Attachment. Kosten +4 Byte/px. **Empfehlung B**: kein Layout-Tanz auf der Tiefe,
die Location-3-Ausgabe wird konsumiert statt verworfen, und Decals/SSR/SSAO lesen dasselbe
GB3. Der eingebaute G-Buffer-Shader muss GB3 dann auch schreiben.

**Vulkan-Pass-Aufteilung.** Heute ist der Himmel der erste Draw in `m_postFxSceneRP`. Für
Deferred: G-Buffer-Pass (eigener RP) → Resolve + Himmel + Schwanz in einem HDR-Pass mit
`loadOp = LOAD` für die Tiefe, oder Himmel hinter den Resolve wie Metal/GL. Wer das
entscheidet, muss die Pipeline-Kompatibilität mitnehmen: `LOAD`-Varianten sind eigene
Render-Pass-Objekte, die Pipelines aber kompatibel, solange die Attachment-Formate gleich
bleiben. **Korrektur aus Schritt 4:** auch die Subpass-Abhängigkeiten müssen gleich sein, sie
zählen zur Kompatibilität (10.11).

### 10.6 Abweichungen, die schon jetzt feststehen

- **Kein Tile/Single-Pass (P6)** auf D3D11/D3D12/Vulkan. D3D hat kein Äquivalent; Vulkan
  könnte es über Subpass-Input-Attachments, das lohnt nur auf TBDR und ist nicht Teil.
- **Eingebaute Materialien sehen im Deferred-Pfad anders aus als im Forward-Pfad.** Der
  eingebaute Forward-Shader ist auf diesen drei Backends eine eigene Implementierung mit
  dokumentierter Drift: Umgebungslicht analytisch per `skyColor()` statt SkyEnv-Cube
  (D11:734, :745), Vulkan zusätzlich ohne SkyEnv, `uAmbient` und Wetter
  (`shaders/scene.frag:38–46`). Der Resolve shadet mit `heLitP`. Deferred-Bilder eingebauter
  Materialien entsprechen damit Graph-Materialien bzw. Metal/GL, nicht dem eigenen
  Forward-Bild. Das P4-Gate (mittlere Abweichung < 1/255) ist deshalb nur mit
  Graph-Materialien aussagekräftig. Die Differenz bei eingebauten Materialien wird gemessen
  und hier benannt, nicht als Fehler gewertet.
- **Cloud-Shadow-Map fehlt** auf allen drei (t33/Binding 33 ist ein Null- bzw. Weiß-View,
  Gate 0; D12:8280–8283, VK:7276–7277). Deferred zeigt keine Wolkenschatten, Forward heute
  auch nicht.
- **GI-Reflexionen** (eigenes Feature neben DDGI, `docs/gi-reflections-plan.md`) fehlen auf
  allen drei auch im Forward-Pfad (`supportsGIReflections` nirgends gesetzt). Hier nicht
  nachgezogen: das Thema bringt den Deferred-Pfad auf diese Backends, nicht Features, die
  dort schon Forward fehlen. Das „GI“ im Thema ist mit DDGI + GI-Masken im Resolve abgedeckt.
- **Skinned bleibt Forward**, wie auf Metal und GL.
- **Gepackte Spiele:** die Spiel-Variante baut mit Cross-Compiler (`CMakeLists.txt:636–670`),
  Resolve und G-Buffer-Varianten werden zur Laufzeit übersetzt wie auf Metal/GL. Eine
  gebackene G-Buffer-Variante gibt es in `MaterialShaderVariant` nicht (`Assets.h:14–43`); sie
  wäre nur ein Mittel gegen den Hänger beim ersten Deferred-Frame, keine Voraussetzung.

### 10.7 Nicht abschreiben: Auffälligkeiten in der Referenz

Gelesen, nicht gemessen. Für die Ports gilt jeweils die rechte Spalte, an Metal/GL selbst wird in
diesem Thema nichts geändert.

| # | Befund | Beleg | Für D3D/Vulkan |
|---|---|---|---|
| 1 | Der Metal-Two-Pass-Resolve nullt `specAA[1]` nicht, `heLitP` wendet Specular-AA dann auch im Fullscreen-Pass an. Der Tile-Resolve und GL nullen. | MTL:13541–13594 vs. :14319; GL:11741–11747; MSL:506, :657 | wie GL nullen |
| 2 | Metal-Two-Pass: `EncodeVelocity` läuft vor dem Depth-Blit und testet gegen die alte Tiefe. GL macht es richtig. | MTL:16224 vs. :16236–16251; GL:11982–11991 | Velocity nach dem G-Buffer-Pass |
| 3 | Jitter uneinheitlich: Metal-Tile-Resolve und Metal-Decals rekonstruieren mit der ungejitterten Matrix, Metal-Two-Pass und GL mit der gejitterten. | MTL:14305–14308, :14433 vs. :13563; GL:11773 | gejittert, wie gerastert |
| 4 | Der GL-Programm-Cache hat kein G-Buffer-Salz, er verlässt sich auf den abweichenden Quell-Hash. | GL:3665–3672; MSL:989 | eigener Schlüsselraum |
| 5 | `ssrComposite` für HLSL ungepinnt. | MSL:1948–1972 | Pin-Tabelle in Schritt 5 |

Veraltete Kommentare, nur notiert: D11:6120–6128 (`heSSRFwd` sei ungepinnt auf s31, die Library
pinnt t31/s8, MSL:2656); VAL:81–85, :90, :120–122 (Zeilennummern); `MetalRenderer.h:397` („GL
ignores decals“); `MaterialShaderLibrary.h:416` und MSL:1188–1189 (GI-Local-Masken würden
nicht auf Cluster-Lichter angewandt, MSL:1240–1248 tut es); D12:10984–10987 (SSR nur im
Editor-Viewport, seit PR #89 überholt, Thema 147 räumt das auf); die Deferred- und
Schatten-Zeilen der Matrix in `backend-parity-plan.md` §2.

### 10.8 Bauplan Schritte 2–6

Reihenfolge D3D11 → D3D12 → Vulkan wie im Thema. D3D11 ist Pilot, weil es keine PSOs und keine
Render-Pass-Objekte hat und der WARP-Zeuge dort am billigsten ist. Umfang laut 116 §4.2:
D3D11 ≈ 700–900, D3D12 ≈ 900–1 200, Vulkan ≈ 1 000–1 300 Zeilen.

**Schritt 2 — D3D11 + gemeinsame Vorarbeit.** 10.4 Punkte 1–4 (Pins, Clustered-Sperre, Tests,
eingebauter `GBufPS`). Dann Targets, G-Buffer-Pass mit Routing (Translucent, ohne GB-Variante,
Skinned → Schwanz), Resolve 8-Licht und Clustered, DSV-Tanz nach Decal-Muster, Schwanz,
Velocity danach, `specAA[1] = 0`, Flag, `HE_DUMP_GBUFFER`/`depthParams.w`.
*Validator-Eintrag:* `GBufPS` in `HLSL_ENTRY_PROFILE` (VAL:101–115).
*Gate:* Windows-CI grün inkl. FXC-Registertest; WARP-Pixeltest in he_tests mit eigenem kleinen
Rig (Muster Clustered-Test TMG:4889): Graph-Material Forward vs. Deferred mittlere Abweichung
< 1/255, dazu > 8 Punktlichter clustered; Log-Zeile „deferred path ready“.

**Schritt 3 — D3D12.** Dieselben HLSL-Quellen und Pins. Resolve-Root-Signatur,
PSO-Dimension `gbuffer`, Barrieren, Heap-Belegung, `GBufPS` neben D12s `kSceneHLSL`.
*Validator-Eintrag:* wie Schritt 2.
*Gate:* WARP-PSO-Test (Signatur deckt Resolve und G-Buffer-PSOs, Muster TMG:3819/4052) +
WARP-Pixeltest wie Schritt 2.

**Schritt 4 — Vulkan.** G-Buffer-RP + Framebuffer mit GB3 (Weg B), Aufteilung von
`m_postFxSceneRP` um den Himmel, dritter Pipeline-Satz, Resolve-Set-Layout, Skinned vor
Transparenz, `shaders/gbuffer.frag` in die glslc-Liste (`src/HE_Rendering/CMakeLists.txt:135–148`).
Abhängigkeiten: Thema 143 (Binding 14 fehlt in VML) und Thema 144 (`mat_ubo`,
`vkCmdUpdateBuffer` im Render-Pass, VK:7362–7379 und :7527–7544) — ohne Fix bleibt der neue
Bildtest auf der Allowlist.
*Gate:* SPIR-V-Reflexionstest Resolve gegen Layout; lavapipe-Fall `deferred` in
`he_vk_imagetests.py`. Das Skript kann heute nur „mittlere Abweichung **mindestens** X“
(`he_vk_imagetests.py:340`); für „Deferred ≈ Forward“ braucht es ein Höchstwert-Kriterium.

**Schritt 5 — Zusatzpässe.** Decals in GB0 (`decalFragmentSampled`), SSAO aus der G-Buffer-Tiefe
(P5), SSR deferred (Trace aus GB1 + Tiefe, `ssrComposite` gepinnt, `ssr.w = 1`), GI im Resolve
gegen GI-an/aus prüfen, Spielpfad: der Spielzweig fährt den Viewport-Frame und erbt den Pfad;
Beleg mit `HE_CAPTURE_FRAME` bzw. exportiertem Spiel.

**Schritt 6 — Bildtests, Parität, Doku.** WARP-Pixeltests D3D11/D3D12, lavapipe-Fälle
(Forward/Deferred, G-Buffer-Ansichten, > 8 Lichter), Abweichungstabelle hier (10.6 + Messwerte),
Matrix in `backend-parity-plan.md` nachziehen. Ein End-to-End-Lauf des Editors auf WARP bräuchte
einen neuen Renderer-Schalter (es gibt heute keinen); ob der sich lohnt, entscheidet Schritt 6.
**Echte-Hardware-Abnahme bleibt offen** und wird im Befund so ausgewiesen.

### 10.9 Schritt 2 umgesetzt: D3D11 (Stand 2026-10-08)

> Zweig wie oben, Commits ab `70ab9cd9`. Gemessen auf NN-WS03 (RTX 4070, Release-Build in einem
> privaten Deploy, eigenes APPDATA je Lauf) und auf WARP in `he_tests`. „Echte Hardware“ heißt hier:
> diese eine NVIDIA-Karte. AMD, Intel und andere Treiber hat der Pfad nicht gesehen.

**Was gebaut ist.**

- `MaterialShaderLibrary`:
  - `deferredResolve[Clustered](HLSL)` ist gepinnt (`compileResolveVariant`). Die Präambel liegt
    auf den Registern von `kHlslMaterialPins` (b0, t10..t18, t31..t33 mit den verschobenen
    Samplern), `HeResolve` auf **b4**, der G-Buffer auf **t27..t30 / s2, s4, s5, s6**. Der Vertrag
    steht als `kHlslResolve*` im Header, Renderer und Tests lesen dieselben Konstanten.
  - Die Clustered-Sperre ist für HLSL und SPIR-V offen, für GLSL 4.1 bleibt sie zu.
  - Metal ist unverändert: Der Metal-Zweig von `compileResolveVariant` ist textgleich, und
    `buildDeferredResolveSource` liefert für Metal dieselben Bytes.
- `LightPacking`: Neu ist `FillMaterialDirectionalWindow`, das Lichtfenster des clustered Resolve
  mit nur den Richtungslichtern. Es ist Metals `EncodeClusterData` als gemeinsame Funktion für
  D3D11, D3D12 und Vulkan.
- `D3D11Renderer`:
  - `GBufPS` steht neben `PSMain` in `kSceneHLSL` und wird von `VSMain` und `VSMainInstanced`
    gespeist.
  - Der G-Buffer (GB0 `R8G8B8A8_UNORM_SRGB`, GB1/GB2 `R16G16B16A16_FLOAT`) wird beim ersten
    Deferred-Frame in Szenengröße angelegt. Die Tiefe ist die vorhandene typeless Szenentiefe:
    beim Schreiben als DSV, im Resolve als SRV. Die DSV kommt vom Output-Merger, bevor die SRV
    gebunden wird, wie im Decal-Pass.
  - Routing im Draw-Loop:
    - Ein Graph-Material mit G-Buffer-Variante geht in den G-Buffer.
    - Ein Graph-Material ohne Variante, oder mit einer, die sich nicht bauen lässt, wird nach dem
      Resolve forward gezeichnet.
    - Eingebaute Materialien laufen über `GBufPS`.
    - Skinned, Decals, Transparenz und Debug-Linien bleiben im Forward-Schwanz.
  - Der Resolve hat einen **eigenen** Lighting-CB: die Material-Füllung des Frames mit
    `specAA[1] = 0`. Im clustered Fall kommt das Fenster aus `FillMaterialDirectionalWindow`.
    Danach bekommt der Built-in-Pass seine Register t15..t18, s1, s2, s3 und b0 zurück.
  - G-Buffer-Varianten der Materialien:
    - Sie haben einen eigenen Schlüsselraum (10.7 #4).
    - Sie werden nur querkompiliert, nie aus einer gebackenen Pak-Variante.
    - Der Warmup baut sie mit.
  - Velocity läuft nach dem G-Buffer-Pass gegen die gefüllte Tiefe (10.7 #2). Der Resolve
    rekonstruiert mit der gejitterten Matrix (10.7 #3).
  - `supportsDeferredRendering` wird beim Init gebaut, wie die GI-Kernel. Der Editor liest das Flag
    beim Start und wendet ein gespeichertes „Deferred“ nur an, wenn es dann schon `true` ist.
  - `HE_RENDER_PATH` und `HE_DUMP_GBUFFER` verhalten sich wie bei GL.
  - Belegzeilen im Log: `D3D11Renderer: deferred path ready (G-buffer + clustered resolve)` und
    `D3D11Renderer: deferred frame (…)`.
- Spielpfad: `GameApplication` liest `RenderPath` aus der config.json, prüft das Flag und fährt über
  `SetSwapchainPostProcessing` den Viewport-Frame. Damit erbt es den Pfad (Beleg unten). Forward
  bleiben der direkte Swapchain-Zweig ohne HDR-Ziel und `RenderWorldPreview`.
- Validator: `GBufPS` steht in `HLSL_ENTRY_PROFILE`.

**Abweichungen vom Bauplan, mit Grund.**

1. **G-Buffer auf t27..t30 statt t19..t22.** Der eingebaute Szenenshader von D3D11 hält seine
   strukturierten Cluster-Listen auf t18..t20. Eine G-Buffer-Ansicht auf t19 oder t20 würde der
   nächste Built-in-Draw (Skinned, Transparenz) still als Cluster-Gitter lesen. SRVs sind nicht
   knapp. Die Sampler bleiben die vorgeschlagenen, und D3D12 erbt denselben Vertrag.
2. **Clustered Resolve mit explizitem LOD.** FXC lehnte `deferredResolveClustered(HLSL)` ab
   (Gradient-Sample in der Lichtschleife, deren Länge pro Pixel variiert, X3570 und dann Abbruch).
   Gemessen auf der RTX: Der erste Lauf meldete `resolveClusteredPS compile failed` und blieb beim
   8-Licht-Fenster. `explicitLodClusterSamples` schreibt die zwei Samples deshalb auf
   `textureLod(…, 0.0)` um, genau wie im Forward-Zwilling. Das geschieht nur im **gebauten** Text
   für Nicht-Metal. Das Literal bleibt, also bleiben auch der Drift-Wächter in `test_culling.cpp`
   und Metals Bytes unverändert.
3. **Basisfarbe eingebauter Materialien im G-Buffer nach der GL/Metal-Regel.** Ohne Material
   0,55 grau, unter einer Textur 1,0 × Textur, der Instanz-Tint wird multipliziert. Das gilt nur im
   G-Buffer, D3D11 forward bleibt unverändert. Forward gibt einem Mesh ohne Material das Weiß des
   `RenderObject`-Defaults und lässt die Textur `uColor` ersetzen. Gemessen: Der
   D3D11-Deferred-Boden der `SHADOWINSTTEST`-Szene lag damit 22,7/255 über GL-Deferred, mit der
   Regel sind es 11,3/255. Den Rest erklärt der nächste Abschnitt.
4. **GB3 wird nicht gebunden.** Der G-Buffer-Tail der Graph-Materialien schreibt `oGB3` auf
   `SV_Target3`. Ohne viertes Ziel verwirft D3D11 das. Der WARP-Debug-Layer meldet dazu nichts.

**Messungen auf der RTX 4070.** Headless aufgenommen (`HE_DUMP_*`), `HE_SKY_TIME=6.2832`. AA,
Bloom, DOF und Motion Blur sind aus, soweit nicht anders genannt. Die Bildwerte stammen aus dem Band
y = 200..720, weil der animierte Himmel darüber liegt. Die Kugelwerte stammen aus der Scheibe der
Graph-Material-Kugel.

| Szene | Vergleich | mittlere Abw. | max |
|---|---|--:|--:|
| `MATERIALTEST=matte` (Graph-Kugel) | D3D11 forward ↔ deferred, Bild | 0,031/255 | 2 |
| dieselbe | Kugel | 0,167/255 | 2 |
| `MANYLIGHTS=16` (Graph-Boden, 16 Punktlichter) | forward ↔ deferred, clustered | 0,013/255 | 1 |
| dieselbe, `HE_FORWARD_CLUSTER=0` | forward ↔ deferred, 8-Licht-Fenster | 0,006/255 | 1 |
| dieselbe | deferred clustered ↔ deferred 8-Licht (Gegenprobe) | 3,874/255 | 65 |
| `MATERIALTEST=1 GIBLEED=3`, GI an | Kugel forward ↔ deferred | 0,175/255 | 2 |
| dieselbe | Kugel deferred, GI an ↔ aus (Gegenprobe) | 7,232/255 | 79 |
| dieselbe, TAA | Kugel forward ↔ deferred | 0,163/255 | 3 |
| dieselbe, SSAO, TOD 0,26 | Kugel forward ↔ deferred | 0,196/255 | 8 |
| `MATERIALTEST=matte` | GL ↔ D3D11, forward | 2,495/255 | 38 |
| dieselbe | GL ↔ D3D11, deferred | 2,493/255 | 32 |
| `SHADOWINSTTEST` (nur eingebaute Materialien) | GL deferred ↔ D3D11 deferred | 11,3/255 | 59 |
| dieselbe | D3D11 forward ↔ D3D11 deferred | 12,4/255 | 69 |
| `LOCALSHADOW=point` (Graph-Boden, Punktlicht mit Atlas-Schatten, TOD 0) | GL deferred ↔ D3D11 deferred | 0,003/255 | 45 |
| `LOCALSHADOW=spot` (Spotlicht mit Atlas-Schatten) | GL deferred ↔ D3D11 deferred | 0,004/255 | 41 |
| beide | GL forward ↔ GL deferred (Referenz) | 0,030 / 0,023 | 106 / 40 |
| beide | D3D11 forward ↔ D3D11 deferred | 15,7 / 37,8 | 151 / 93 |
| `LANDSCAPELAYERS=1` (Graph-Terrain, Weightmap t14/s0) | D3D11 forward ↔ deferred | 0,235/255 | 12 |

Die Gegenproben zeigen:
- Der clustered Resolve lichtet alle 16 Lichter, nicht nur das Fenster.
- GI wirkt im Resolve.

G-Buffer-Ansichten (`HE_DUMP_GBUFFER=1..4`) in `SHADOWINSTTEST`, GL ↔ D3D11:
- Normale und Rough/Spec stimmen überein.
- BaseColor lag vor Punkt 3 bei 209 gegen 231. Nach Punkt 3 neu gemessen ist sie an vier
  Messpunkten byte-gleich (209,209,209).

Point/Spot-Atlas-Schatten (`heClusterShadow` mit den D3D-gebackenen `localShadowVP`):
- Sie treffen GL deferred auf 0,003–0,004/255.
- An den Messpunkten sind GL forward, GL deferred und D3D11 deferred byte-gleich, außerhalb des
  Kegels (157,168,192).
- D3D11 **forward** liegt dort bei (105,116,144), auch mit `HE_DUMP_SSAO=0`. Abweichend ist also
  D3D11 forward, nicht der neue Pfad (siehe unten).

Die GL- und die D3D11-Aufnahmen rendern ihre Settle-Frames auf dieselbe Weise innerhalb des Dumps;
die Session-Zusammenfassung meldet auf beiden „1 frames“.

**Spielpfad.** Ein exportiertes D3D11-Spiel wurde auf der RTX gestartet: das Depthy-Projekt aus Thema 130 mit den Binärdateien dieses Zweigs, Fenster 1600×900, Aufnahme per `PrintWindow` über `docs/spielpfad-postfx-run-game.ps1`. Mit `RenderPath=1` in der config.json loggt es `swapchain post chain active (1600x900)` und dann `deferred frame (1600x900, clustered resolve, …)`. Mit `RenderPath=0` erscheint keine Deferred-Zeile. Das Bild zeigt Geometrie, CSM-Schatten und Himmel vollständig. Die eingebauten Würfel tragen den heLitP-Look (siehe die Abweichungen zu Metal/GL unten).

**WARP (`he_tests`).**

- „D3D11: G-buffer + deferred resolve shade a graph material like its forward draw“, ein 8×8-Bild
  mit gekipptem Dreieck und außermittigem Punktlicht:
  - Forward ↔ deferred: Mittel 0,0021, Maximum 0,0048, bei einer HDR-Luminanz von 0,85..2,29.
  - Clustered (12 Listenlichter hinter einem Fenster aus 8 schwarzen): Mittel 0,0016, Maximum
    0,0044.
- Gegenproben:
  - Falsches uv-Vorzeichen: 0,073 im Mittel, 0,217 im Maximum.
  - 8-Licht-Resolve über demselben G-Buffer: 0,34.
  - Ungekürztes Fenster mit leuchtenden Lichtern: 0,34. Das ist die Doppelzählung, die
    `FillMaterialDirectionalWindow` verhindert.
  - Die BaseColor-Ansicht zeigt 0,25 / 0,5 / 0,75.
- Die Reflexion der Resolve-Bytecodes zeigt b0, b4, t27..t30 und s2/s4/s5/s6, im clustered Fall
  zusätzlich t24..t26 als ByteAddressBuffer.
- „Deferred resolve HLSL registers stay inside SM 5.0's bindable range“ prüft den Text auf allen
  Plattformen.

**Offline-Prüfung der eingebetteten Shader.**

- `glslangValidator -D -V -S frag -e GBufPS` auf `kSkyFuncHLSL + kSceneHLSL`: rc 0. Mit
  verfälschtem Funktionsnamen (Negativkontrolle): rc 2.
- `scripts/validate_embedded_shaders.py` mit FXC: 52 HLSL- und 57 GLSL-Shader übersetzt, 0
  gescheitert. Mit verfälschtem `GBufPS`: exit 1.

**Abweichungen zu Metal/GL (D3D11).**

- *Eingebaute Materialien* sehen in D3D11 deferred anders aus als in D3D11 forward (siehe 10.6),
  gemessen 12,4/255 in `SHADOWINSTTEST`. Wer auf D3D11 den Pfad umschaltet, sieht eingebaute
  Materialien heller und stärker vom Himmel getönt. So sehen sie auf GL und Metal in beiden Pfaden
  aus.
- *D3D11 ↔ GL bei Graph-Materialien* (vorbestehend, Ursache offen):
  - In `MATERIALTEST` ist die Kugel auf D3D11 heller als auf GL: 224 → 236 im 8-Bit-Bild, 2,5/255
    bildweit. Forward und deferred zeigen das gleich, der Deferred-Pfad reicht den Unterschied nur
    durch.
  - Der Rest von 11,3/255 bei eingebauten Materialien in `SHADOWINSTTEST` geht in dieselbe
    Richtung: Der Boden liegt um 18 höher.
  - In `LOCALSHADOW` ist es umgekehrt: D3D11 deferred = GL, D3D11 forward ist außerhalb des
    Lichtkegels dunkler, 105 statt 157, unabhängig von SSAO.
  - Die Ursachen sind nicht untersucht. Sie gehören zur Graph-Material-Parität von D3D11 forward,
    die dieses Thema ausschließt. Für einen eigenen Befund reproduzieren die Witness-Szenen oben
    beide Fälle in Sekunden.
- Ebenfalls vorbestehend: D3D11 füllt `lit.weather`, `lit.specAA` und `lit.viewMode` nicht.
  Nässe/Schnee und Specular-AA fehlen also auch im Resolve, genau wie bei D3D11-Graph-Materialien im
  Forward-Pfad.
- *SSR* läuft in einem Deferred-Frame nicht. *Decals* bleiben selbst beleuchtet über dem Resolve,
  *SSAO* liest das Prepass-Ergebnis. All das ist Schritt 5. **Erledigt in Schritt 5:** SSR und
  Decals in GB0, SSAO bleibt beim Prepass, siehe 10.12.
- *Debug-Layer*: Der D3D11-Renderer legt kein Debug-Gerät an (`HE_GPU_DEBUG` wirkt auf D3D11
  nicht). Laufzeitmeldungen hat nur WARP in `he_tests` gesehen.

### 10.10 Schritt 3 umgesetzt: D3D12 (Stand 2026-10-08)

> Zweig wie oben, Code-Commit `9d2d6c54`. Gemessen auf NN-WS03 (RTX 4070, Release-Build in einem
> privaten Deploy, eigenes APPDATA je Lauf, **D3D12-Debug-Layer an** über `HE_GPU_DEBUG=1`) und auf
> WARP in `he_tests`. „Echte Hardware“ heißt auch hier nur diese eine NVIDIA-Karte.

**Was gebaut ist.** Dieselben Passes und dieselben Shader wie D3D11 (10.9). Der Resolve und die
G-Buffer-Tails der Graph-Materialien kommen unverändert aus `MaterialShaderLibrary`
(`deferredResolve[Clustered](HLSL)`, `fullscreenVertex`, `resolveGBufferShaders`). Nichts davon ist
für D3D12 kopiert. Die eine Ausnahme ist `GBufPS` für eingebaute Materialien: Es ist eine zweite
Handkopie neben D3D11s, mit Absicht nach 10.4 Punkt 4. Sie liest den `PerObject`-Puffer und die
Textur des jeweiligen `kSceneHLSL`, und die Namen unterscheiden sich (`uAlbedo`/`uAlbedoSamp` gegen
`uTexture`/`uSampler`). Kein Drift-Wächter hält die beiden zehn Zeilen zusammen; die G-Buffer-
Ansichten 1..4 gegen GL (Tabelle unten) sind der Laufzeitbeleg. Neu ist sonst nur, was D3D12
anders verlangt:

- **Resolve-Root-Signatur** (`HE::d3d12mat::DescribeResolveRootSignature` in
  `D3D12MaterialRootSignature.h`, Renderer und `he_tests` lesen dieselbe Beschreibung):
  - Sie ist die Material-Signatur plus `HeResolve` als Root-CBV b4 (Param 9) plus t27..t30 als
    vier weitere Slots in **derselben** Tabelle (Slots 17..20 hinter dem 17er-Materialblock).
  - Params 0..8 behalten ihre Indizes, `bindClusterRoots` setzt die Cluster-Listen t24..t26
    weiter auf 6..8.
  - **Eigene Signatur, nicht `m_matRootSig`:** Statische Sampler hängen fest am Register, und
    die Material-Signatur legt auf s2/s4/s5/s6 linear-WRAP (für `heTex0`/`heTexP*`). Der Resolve
    braucht dort point-clamp wie auf D3D11. Ein wrappender Tiefen-Lookup am Bildrand liest sonst
    die gegenüberliegende Kante.
- **Deskriptoren:** Die Resolve-Tabelle sind zwei aufeinanderfolgende Blöcke des Material-Rings
  (`m_matSrvHeap`), reserviert in dem Moment, in dem der Frame sich für Deferred entscheidet. Ein
  Frame, der den G-Buffer schon gefüllt hat, kann also immer auflösen. Inhalt: die
  Material-Vorlage (GI-Masken, CSM, Local-Atlas, DDGI-Atlanten), Sky-Cube und SSAO unter denselben
  Gates wie ein Graph-Material-Draw, dann GB0..2 und eine `R32_FLOAT`-Sicht der Viewport-Tiefe.
  Damit liegt alles in dem einen shader-sichtbaren Heap, den der Frame-Fence schon schützt.
- **Konstanten:** eigener `HeLighting`- und `HeResolve`-Puffer je Frame-Slot. Upload-Speicher wird
  erst bei der Ausführung gelesen. Würde der Resolve sein gekürztes Fenster (`specAA[1] = 0`,
  im clustered Fall nur Richtungslichter) in `m_matLightCB` schreiben, bekämen Forward-Replay
  und Transparenz im selben Frame dieses Fenster.
- **G-Buffer-PSOs:** `GBufPS` neben `PSMain` im D3D12-`kSceneHLSL` (D3D11s Regel auf
  `uAlbedo`/`uAlbedoSamp`), mit `VSMain` und `VSMainInstanced` auf der Szenen-Signatur, drei MRTs
  gegen D32. Graph-Materialien bekommen die PSO-Dimension `gbuffer`. Sie hat einen eigenen
  Schlüsselraum im PSO-Cache **und** im FXC-Bytecode-Cache, der nur nach dem Hash geht. Sie wird
  nie aus einer gebackenen Pak-Variante gebaut und nie clustered.
- **Ablauf und Barrieren:**
  - Ablauf: G-Buffer-Pass, Resolve, Forward-Replay der Graph-Materialien ohne G-Buffer-Variante.
    Danach Skinned, Velocity, Decals, Transparenz und Debug-Linien unverändert.
  - Vor dem Resolve wie in `EncodeDecals`: erst die DSV vom Output-Merger, dann die Tiefe
    DEPTH_WRITE → PIXEL_SHADER_RESOURCE und der G-Buffer RENDER_TARGET → PIXEL_SHADER_RESOURCE.
  - Danach zurück, und erst dann kommt die Tiefe wieder auf den OM.
  - Der instanzierte Built-in-Zweig stellt jetzt den PSO des laufenden Passes wieder her
    (`activeScenePso`), nicht hart den Forward-PSO. Im G-Buffer-Pass stünde sonst ein PSO mit
    einem Ziel auf drei gebundenen Zielen.
- **Zerstörung und Größe** (`docs/d3d12-swapchain-resize-befund.md`):
  - Der G-Buffer wird lazy in Szenengröße angelegt. Bei einer Größenänderung kommt erst
    `waitForAllFrames()` (die Regel von `createSSAOTargets`), dann die neuen Ziele.
  - Die optimierten Clear-Werte sind die Clears des Passes, also GB1 = (0,5; 0,5; 1; 0,5).
  - `Shutdown` gibt alles bei leerem Gerät frei.
- **Flag und Schalter:**
  - `supportsDeferredRendering = postFxReady && deferredReady`.
  - Gebaut wird nach `createPostFXPipelines()`. Auf D3D12 entstehen die PostFX-Pipelines erst nach
    `createPipeline`, anders als auf D3D11.
  - `HE_RENDER_PATH` und `HE_DUMP_GBUFFER` wie auf GL und D3D11. Der Warmup baut die
    G-Buffer-Varianten mit.
  - Der Deferred-Pfad gilt nur im HDR-Viewport-Frame (`usingHDR`). Den fährt der Editor-Viewport,
    und das exportierte Spiel fährt ihn über `SetSwapchainPostProcessing`.
- **SSR** läuft im Deferred-Frame nicht, wie auf D3D11 (Schritt 5). Erledigt in Schritt 5, 10.12.
- Belegzeilen im Log: `D3D12Renderer: deferred path ready (G-buffer + clustered resolve)` und
  `D3D12Renderer: deferred frame (…)`.
- Validator: Der `GBufPS`-Eintrag aus Schritt 2 deckt die D3D12-Kopie mit ab (53 HLSL-Shader, 0
  gescheitert).

**GB3 ist nicht gebunden.** Der G-Buffer-Tail schreibt `oGB3` auf `SV_Target3`. Der D3D12-Debug-
Layer sagt dazu bei jeder G-Buffer-Material-PSO einmal: „expects a Render Target View bound to
slot 3 … This is OK, as writes of an unbound Render Target View are discarded“. Das ist eine
Info-Warnung, kein Fehler, auf WARP und auf der RTX gleich. Wer sie loswerden will, bindet GB3 als
viertes Ziel (Weg B aus 10.5, den Vulkan ohnehin nehmen soll).

**Messungen auf der RTX 4070.**
- Headless (`HE_DUMP_*`), `HE_SKY_TIME=6.2832`, AA/Bloom/DOF/Motion Blur aus, Wolken aus.
- Szenen und Kamera:
  - Die Graph-Kugel (`MATERIALTEST`, GI) wird mit `SKYTEST` bei TOD 0,45 aufgenommen.
  - `MANYLIGHTS` und `LOCALSHADOW` bei TOD 0 mit CAMY 207 / CAMZ 2 / PITCH −38.
  - `LANDSCAPELAYERS` mit CAMY 340 / CAMZ 70 / PITCH −35.
- **Ohne `SKYTEST` ist es Mitternacht ohne Himmel**, und die Kamera-Knöpfe wirken nicht. Die
  ersten Aufnahmen dieses Schritts waren deshalb schwarz oder leer (`draws=0`), mit dem Ergebnis
  „forward = deferred = D3D11“ zum Byte.
- Gemessen wird im Band y = 200..720, die Kugelwerte in der Scheibe um die Bildmitte (r = 190).
- Skripte: `scripts/deferred-witness/` (`cap.ps1` = eine Aufnahme aus dem privaten Deploy, `run12*.ps1`
  = die Reihen mit allen Knöpfen, `ana12.py`/`img.py` = Band- und Kugelvergleich). Die Pfade zeigen auf
  `C:\hw150` auf NN-WS03; für einen anderen Baum `$exe`/`$out` in `cap.ps1` umstellen.

| Szene | Vergleich | mittlere Abw. | max |
|---|---|--:|--:|
| `MATERIALTEST=matte` (Graph-Kugel) | D3D12 forward ↔ deferred, Bild | 0,037/255 | 3 |
| dieselbe | Kugel | 0,202/255 | 1 |
| dieselbe | D3D11 deferred ↔ D3D12 deferred | 0,000/255 | 1 |
| dieselbe | GL deferred ↔ D3D12 deferred, Bild / Kugel | 0,054 / 0,309 | 4 |
| `MANYLIGHTS=16` (Graph-Boden, 16 Punktlichter) | D3D12 forward ↔ deferred, clustered | 0,013/255 | 1 |
| dieselbe, `HE_FORWARD_CLUSTER=0` | forward ↔ deferred, 8-Licht-Fenster | 0,006/255 | 1 |
| dieselbe | deferred clustered ↔ deferred 8-Licht (Gegenprobe) | 3,881/255 | 65 |
| dieselbe | D3D11 deferred ↔ D3D12 deferred | 0,000/255 | 0 |
| `MATERIALTEST=1 GIBLEED=3`, GI an | Kugel forward ↔ deferred | 0,203/255 | 5 |
| dieselbe | Kugel deferred, GI an ↔ aus (Gegenprobe) | 28,0/255 | 130 |
| `LOCALSHADOW=point` (Graph-Boden, Atlas-Schatten) | GL deferred ↔ D3D12 deferred | 0,003/255 | 63 |
| `LOCALSHADOW=spot` | GL deferred ↔ D3D12 deferred | 0,005/255 | 37 |
| `LOCALSHADOW=point` | D3D11 deferred ↔ D3D12 deferred | 0,000/255 | 0 |
| `LOCALSHADOW=point` | GL forward ↔ GL deferred (Referenz) | 0,027/255 | 48 |
| `LOCALSHADOW=point` / `spot` | D3D12 forward ↔ D3D12 deferred | 15,8 / 36,6 | 115 / 61 |
| `LANDSCAPELAYERS=1` | D3D12 forward ↔ deferred | 0,043/255 | 16 |
| `SHADOWINSTTEST` (nur eingebaute Materialien) | GL deferred ↔ D3D12 deferred | 0,038/255 | 12 |
| dieselbe | D3D12 forward ↔ D3D12 deferred | 2,35/255 | 28 |
| dieselbe, G-Buffer-Ansicht 1..4 | GL ↔ D3D12 | 0,000 / 0,012 / 0,210 / 0,629 | 1 / 4 / 4 / 4 |
| `MATERIALTEST=1 GIBLEED=3`, GI an, **TAA** (`AA=3`) | Kugel forward ↔ deferred | 0,196/255 | 3 |
| `SSRTEST` + `MATERIALTEST=matte` (Kugel auf dem Boden), TOD 0,26, SSR aus | **SSAO** an ↔ aus, deferred | 0,215/255 | 30 |
| dieselbe | SSAO an ↔ aus, forward (Referenz) | 0,230/255 | 33 |
| `DECALTEST` (roter Decal auf grauem Boden) | D3D12 forward ↔ deferred | 1,00/255 | 20 |

Was die Tabelle zeigt:
- D3D12 deferred ist mit D3D11 deferred byte- oder fast byte-gleich. Die Shader sind dieselben,
  nur die Bindung ist neu.
- D3D12 deferred trifft GL deferred bei Graph-Materialien, Atlas-Schatten und eingebauten
  Materialien.
- Die Gegenproben zeigen: Der clustered Resolve lichtet alle 16 Lichter, und GI wirkt im Resolve.
- `MANYLIGHTS` gegen GL deferred liegt bei 3,9/255. Das ist genau die Gegenprobe oben: GL hat
  keinen clustered Resolve (10.3), sein Deferred-Bild ist das 8-Licht-Fenster.
- TAA, SSAO und Decals laufen im Deferred-Frame mit:
  - SSAO wirkt im Resolve so stark wie forward. Der Resolve bindet den geblurrten SSAO an `kSlotAO`
    unter demselben Gate wie ein Graph-Material.
  - Die erste AO-Szene (Kugel ohne Boden) war für AO blind: an ↔ aus 0,05/255 forward wie
    deferred. Erst die Kugel auf dem `SSRTEST`-Boden zeigt den Kontaktschatten.
  - Forward ↔ deferred liegt in der `SSRTEST`-Szene bei 35,9/255, weil Boden und Wand eingebaute
    Materialien sind (10.6).
  - Der Decal-Lauf ist der einzige, in dem die Tiefe zweimal hintereinander DEPTH_WRITE ↔
    PIXEL_SHADER_RESOURCE wechselt (Resolve, dann `EncodeDecals`). Er ist mit Debug-Layer
    fehlerfrei. Der selbst beleuchtete Decal liegt über dem Resolve, die 1,0/255 sind der
    eingebaute Boden (10.6).

**Debug-Layer.** In allen 21 D3D12-Editorläufen der Haupttabelle, den zwölf Läufen für TAA, SSAO
und Decals und den drei Spielläufen gab es **0 Fehler**
und keine Device-Removal. Drei Warnungstexte kommen vor:
- „slot 3 … discarded“: einmal je G-Buffer-Material-PSO, siehe oben.
- „ClearRenderTargetView: The clear values do not match …“: vorbestehend, gleich oft in Forward-
  und Deferred-Läufen (Spiel 2/2, `MATERIALTEST` 1/1). Der G-Buffer selbst ist mit seinen
  Clear-Werten angelegt.
- „CreateCommittedResource: Ignoring InitialState UNORDERED_ACCESS“: vorbestehend, im GI-Lauf
  forward wie deferred.

**Spielpfad.** Das Depthy-Spiel aus Thema 130 läuft mit den Binärdateien dieses Zweigs und
`GameBackend=D3D12` (`HorizonRendering.dll` aus dem Build-Baum, siehe Lesson zum veralteten
`deploy/Game`), aufgenommen per `PrintWindow` über `docs/spielpfad-postfx-run-game.ps1`.
- Mit `RenderPath=1` loggt es `swapchain post chain active (2000x1125)` und dann
  `deferred frame (2000x1125, clustered resolve, G-buffer view 0)`. Das Bild zeigt Geometrie,
  CSM-Schatten und Himmel vollständig.
- Mit `RenderPath=0` erscheint keine Deferred-Zeile.
- **Resize** (`-Mode resize`, acht Schritte zwischen 960×540 und 1800×1000, dazu schnelles und
  langsames Ziehen und Wiederherstellen): 173 Resize- und Post-Chain-Zeilen,
  0 Fehler, keine Device-Removal. Jedes Bild ist vollständig, auch das kleinste nach dem großen.

**WARP (`he_tests`).**
- „D3D12: the deferred resolve and G-buffer PSOs build against their signatures“:
  - Die Resolve-Signatur nimmt den 8-Licht- und den clustered Resolve an.
  - Die Material-Signatur lehnt beide mit `E_INVALIDARG` ab (Gegenprobe).
  - Die G-Buffer-Material-PSO mit drei MRTs ist legal.
  - Die Reflexion zeigt t27..t30, t24..t26 als ByteAddressBuffer und b4.
- „D3D12: G-buffer + deferred resolve shade a graph material like its forward draw“, der
  D3D11-Fall auf D3D12: dieselben zwei Signaturen, die 17+4-Tabelle, R32_TYPELESS-Tiefe mit
  D32-DSV und R32_FLOAT-SRV, explizite Barrieren, Cluster-Listen als Root-SRVs.
  - Forward ↔ deferred: Mittel 0,0021, Maximum 0,0048.
  - Clustered: Mittel 0,0016, Maximum 0,0044.
  - Gegenproben: uv-Vorzeichen max 0,217; 8-Licht-Resolve 0,34; die BaseColor-Ansicht zeigt
    0,25 / 0,5 / 0,75.
  - Debug-Layer im Pixeltest: keine Meldung.
- Gesamtlauf `he_tests` (Release): 4188 von 4190 Fällen grün. Die zwei roten sind die bekannten
  Zwischenablage-Fälle in `test_inspector_ui`, vorbestehend und ohne Bezug zu diesem Schritt.

**Abweichungen zu Metal/GL (D3D12).**
- *Eingebaute Materialien* sehen in D3D12 deferred anders aus als in D3D12 forward. Gemessen sind
  2,35/255 in `SHADOWINSTTEST`, und sie entsprechen dann GL (0,038/255). Das ist dieselbe Lage wie
  auf D3D11 (10.6, 10.9).
- *D3D12 forward bei Atlas-Schatten* weicht von GL ab: 15,8/255 gegen GL forward, dasselbe Bild wie
  D3D11 forward in 10.9. Der Deferred-Pfad trifft GL.
- *Landscape-Gewichte fehlen auf D3D12 ganz*, vorbestehend und forward wie deferred.
  - Der Renderer schreibt nie eine Gewichtskarte in den Slot `kSlotLandscapeWeights` (t14), die
    Vorlage hält dort eine Null-Sicht.
  - Ein bemaltes Terrain zeigt deshalb nur Layer 0: 11,3/255 gegen GL und D3D11 deferred in
    `LANDSCAPELAYERS`.
  - D3D11 bindet t14 seit Thema 57 je Draw (`D3D11MaterialBindings.h`). D3D12 hat dafür keine
    Entsprechung.
  - Nicht hier behoben. Das ist eine Lücke der Graph-Material-Parität von D3D12 forward, also
    außerhalb dieses Themas. Für einen eigenen Befund reproduziert der Witness oben sie in
    Sekunden.
- D3D12 füllt wie D3D11 `lit.weather`, `lit.specAA` und `lit.viewMode` nicht. Nässe/Schnee und
  Specular-AA fehlen also auch im Resolve.
- *SSR* läuft im Deferred-Frame nicht. *Decals* bleiben selbst beleuchtet über dem Resolve.
  *SSAO* liest das Prepass-Ergebnis. Das alles ist Schritt 5. **Erledigt in Schritt 5:** SSR und
  Decals in GB0, SSAO bleibt beim Prepass, siehe 10.12.
- *Kein Tile/Single-Pass* (10.6).

### 10.11 Schritt 4 umgesetzt: Vulkan (Stand 2026-10-08)

> Zweig wie oben, auf `origin/release/0.7.0` gemergt (`92f617cb`, von der Queen freigegeben):
> Schritt 4 braucht die Vulkan-Fixes der Themen 143–146 (Binding 14 im Material-Layout,
> Material-UBO als Ring statt `vkCmdUpdateBuffer` im Render-Pass, SSAO-Blur-Layout, ein
> `setDayNight` am Frame-Anfang), die auf der alten Basis `469fa9f6` fehlten. Code-Commit
> `c50cc8a0`. Gemessen auf NN-WS03 (RTX 4070, Release, privater Deploy, eigenes APPDATA je Lauf,
> **Vulkan-Validation-Layer an**, `HE_GPU_DEBUG=1`). „Echte Hardware“ heißt auch hier nur diese
> eine NVIDIA-Karte. lavapipe hat den Pfad noch nicht gesehen: die zwei neuen Fälle laufen erst
> im CI-Job `vulkan-lavapipe` (unten).

**Was gebaut ist.** Dieselben Shader wie auf D3D11/D3D12: `deferredResolve[Clustered](SpirV)`,
`fullscreenVertex(SpirV)` und die G-Buffer-Tails aus `resolveGBufferShaders`, alle aus
`MaterialShaderLibrary`, nichts für Vulkan kopiert. Neu ist, was Vulkans Render-Pass-Modell
verlangt.

- **Drei Szenen-Passes im HDR-Viewport-Frame** (Plan 10.5, „Vulkan-Pass-Aufteilung“):
  1. `m_postFxSceneRP` wie bisher. Darin landet in einem Deferred-Frame nur noch der Himmel (ohne
     Tiefentest); `DrawScene` beendet den Pass, sobald die Opaken an der Reihe sind.
  2. `m_gbufferRP`: vier Farbziele plus die Viewport-Tiefe, alles gelöscht. GB0
     `R8G8B8A8_SRGB`, GB1/GB2 `R16G16B16A16_SFLOAT`, **GB3 `R32_SFLOAT` mit `gl_FragCoord.z`**
     (Weg B aus 10.5): der Resolve sampelt GB3, die Tiefe bleibt reines Attachment, es gibt
     keinen Layout-Tanz auf ihr. Der G-Buffer-Tail schreibt `oGB3` ohnehin; auf Vulkan wird es
     konsumiert statt verworfen. Die Farbziele enden per `finalLayout` in
     `SHADER_READ_ONLY_OPTIMAL`, die Tiefe als Attachment.
  3. `m_hdrLoadRP`: `m_postFxSceneRP` mit `LOAD` auf Farbe (der Himmel) und Tiefe (die des
     G-Buffer-Passes). Darin der Resolve und der Forward-Schwanz: Graph-Materialien ohne
     G-Buffer-Variante (Replay), Decals, Skinned, Transparenz, Debug-Linien.
- **Render-Pass-Kompatibilität, gemessen statt gelesen.** Der erste Lauf auf der RTX hatte ein
  richtiges Bild und 20 Validation-Fehler pro Frame: „pDependencies[0].srcStageMask is
  incompatible between VkRenderPass … (from VkFramebuffer/VkPipeline)“. Subpass-Abhängigkeiten
  gehören zur Kompatibilität (nur Load/Store-Ops und Layouts sind ausgenommen). Die Annahme in
  10.5 war also zu schwach. `m_hdrLoadRP` trägt deshalb die Abhängigkeit von `m_postFxSceneRP`
  wörtlich (beide Stellen mit KEEP-IN-SYNC-Kommentar). Die Ordnung „Himmel und G-Buffer
  geschrieben, bevor der dritte Pass lädt, testet und sampelt“ stellt eine globale
  `VkMemoryBarrier` zwischen den Passes her. Danach: 0 Meldungen in jedem Lauf unten.
- **Resolve-Set** (`HE::vkmat::kResolveBindings` in `VulkanMaterialLayout.h`, Renderer und
  `he_tests` lesen dieselbe Tabelle):
  - 0 (`HeLighting`), 10–13, 15–18 und 31–33 wie das Material-Set, dazu 19–22 (G-Buffer) und 23
    (`HeResolve`), am Ende die Cluster-Listen 24–26.
  - 15 kombinierte Sampler im Fragment-Stage: unter dem Spec-Minimum 16, kein Gerät braucht einen
    Fallback. 14 (`heLandscapeWeights`) gehört nicht dazu, der Resolve deklariert ihn nicht.
  - Ein persistentes Set je Frame-Slot, nach dem Fence des Slots neu beschrieben (wie Binding 8 des
    Szenen-Sets). Dieselben Views, Sampler, Layouts und Gates wie ein Graph-Material-Draw (GI-Maske
    11 und DDGI-Atlanten in `GENERAL`), G-Buffer mit einem eigenen Point-Clamp-Sampler.
  - Eigener `HeLighting`- und `HeResolve`-Puffer je Slot, host-sichtbar und kohärent gemappt.
    Inhalt: die Material-Füllung des Frames mit `specAA[1] = 0`, im clustered Fall das Fenster aus
    `FillMaterialDirectionalWindow`. `m_matLightBuf` bleibt für Replay und Schwanz unberührt.
  - Das `memcpy` in diese Puffer und `vkUpdateDescriptorSets` stehen in der Aufzeichnung
    **innerhalb** von `m_hdrLoadRP`. Die Falle aus Thema 144 tritt trotzdem nicht auf: Das sind
    Host-Schreibzugriffe auf Speicher, den die GPU erst nach dem Submit liest. Es gibt keinen
    Transfer-Befehl (`vkCmdUpdateBuffer`, `vkCmdCopy*`) im Render-Pass. Wer hier einen Transfer
    einführt, baut die Falle wieder ein.
  - `depthParams = (+1, 1, 0)` wie der Decal-Pass, die Inverse der gejitterten `viewProj`
    (`kVulkanClipFix` steckt darin, 10.7 #3).
- **G-Buffer-Pipelines:**
  - `shaders/gbuffer.frag` (in der glslc-Liste) für eingebaute Materialien, mit `scene.vert` und
    `scene_instanced.vert` auf dem Szenen-Layout. Die Basisfarbe folgt der GL/Metal-Regel wie
    auf D3D11/D3D12.
  - Graph-Materialien bekommen in `GetOrBuildMaterialPipeline` die Dimension `gbuffer`: eigener
    Schlüsselraum, nie gebacken, nie clustered, vier identische Blend-States
    (`independentBlend` bleibt aus).
  - Der instanzierte Zweig stellt die Pipeline des laufenden Passes wieder her
    (`activeMatScenePipe`), wie auf D3D12.
- **Skinned vor Transparenz** (10.3), in beiden Pfaden: bisher zeichnete Vulkan Skinned nach den
  Transparenten. **Ungeprüft:**
  - Keine Witness-Szene hat ein Skinned-Mesh. Der Wechsel ist also gelesen, nicht gelaufen,
    weder forward noch deferred.
  - Das gilt auch für das Neubinden von Set 0/2 zwischen `m_skinnedPipeLayout` und dem
    transparenten `drawDCVk`-Pass. Der Skinned-Block stellt am Ende Szenen-Pipeline und Set 0
    wieder her, und der Transparenz-Pass bindet seine Pipeline selbst.
  - Ein Skinned-Witness auf Vulkan gehört in Schritt 6.
- **Größe:** Der G-Buffer wird lazy in Viewport-Größe angelegt und mit den PostFX-Zielen
  freigegeben (`destroyPostFXResources`, GPU dort idle). Sein Framebuffer hält die
  Viewport-Tiefe, also muss er vor ihr gehen.
- **Flag und Schalter:** `supportsDeferredRendering = postFxReady && deferredReady`, gebaut beim
  Init nach Szenen- und PostFX-Pipelines. `HE_RENDER_PATH`/`HE_DUMP_GBUFFER` und der Warmup der
  G-Buffer-Varianten wie auf D3D. Ein Deferred-Frame ist nur der HDR-Viewport-Frame, also der
  Editor-Viewport und das exportierte Spiel über `SetSwapchainPostProcessing`.
- **SSR** läuft im Deferred-Frame nicht, wie auf D3D11/D3D12 (Schritt 5). Erledigt in Schritt 5, 10.12.
- Belegzeilen im Log: `VulkanRenderer: deferred path ready (G-buffer + clustered resolve)` und
  `VulkanRenderer: deferred frame (…)`.
- **Validator:** `gbuffer.frag` ist eine Datei, die glslc beim Build übersetzt. Ein kaputter
  Shader lässt den Build scheitern, der Validator für eingebettete Strings (VAL) ist dafür nicht
  zuständig. Die Laufzeit-SPIR-V des Resolve deckt der Reflexionstest unten.

**Messungen auf der RTX 4070 (Vulkan, Validation an).**
- Szenen, Kamera und Knöpfe wie in 10.10, Skript `scripts/deferred-witness/run_vk.ps1`. Es nimmt
  auch die GL- und D3D12-Referenzen aus demselben Build auf.
- Band = y 200..720, Kugel = Scheibe r 190 um die Bildmitte.
- In allen 38 Vulkan-Editorläufen nach dem Kompatibilitäts-Fix (Layer in jedem Log als „validation
  layer ENABLED“ belegt) gab es **0 Validation-Meldungen** und 0 Fehler. Jeder Lauf hatte
  `draws > 0`, jeder Deferred-Lauf die Zeile `deferred frame`.

| Szene | Vergleich | mittlere Abw. | max |
|---|---|--:|--:|
| `MATERIALTEST=matte` (Graph-Kugel) | Vulkan forward ↔ deferred, Band / Kugel | 0,009 / 0,045 | 4 / 1 |
| dieselbe | GL deferred ↔ Vulkan deferred, Band / Kugel | 0,031 / 0,130 | 3 |
| dieselbe | D3D12 deferred ↔ Vulkan deferred, Band / Kugel | 0,052 / 0,253 | 4 |
| dieselbe | deferred ↔ G-Buffer-Ansicht 1 (Gegenprobe), Kugel | 35,4 | 108 |
| `MANYLIGHTS=16` (Graph-Boden) | Vulkan forward ↔ deferred, clustered | 0,013 | 1 |
| dieselbe, `HE_FORWARD_CLUSTER=0` | forward ↔ deferred, 8-Licht-Fenster | 0,008 | 1 |
| dieselbe | deferred clustered ↔ deferred 8-Licht (Gegenprobe) | 3,876 | 65 |
| dieselbe | D3D12 deferred ↔ Vulkan deferred | 0,007 | 1 |
| dieselbe | GL deferred ↔ Vulkan deferred 8-Licht | 0,047 | 66 |
| `MATERIALTEST=1 GIBLEED=3`, GI an | Kugel forward ↔ deferred | 0,203 | 19 |
| dieselbe | Kugel deferred, GI an ↔ aus (Gegenprobe) | 28,1 | 129 |
| dieselbe | Kugel GL deferred ↔ Vulkan deferred | 0,274 | 19 |
| dieselbe, TAA (`AA=3`) | Kugel forward ↔ deferred | 0,192 | 4 |
| `LOCALSHADOW=point` / `spot` | GL deferred ↔ Vulkan deferred | 0,013 / 0,010 | 107 / 64 |
| `LOCALSHADOW=point` | D3D12 deferred ↔ Vulkan deferred | 0,013 | 107 |
| `LOCALSHADOW=point` / `spot` | Vulkan forward ↔ deferred | 15,8 / 36,6 | 153 / 92 |
| `LOCALSHADOW=point` | GL forward ↔ Vulkan forward | 15,8 | 63 |
| `LANDSCAPELAYERS=1` | Vulkan forward ↔ deferred / GL deferred ↔ Vulkan deferred | 0,053 / 0,045 | 16 / 22 |
| `SHADOWINSTTEST` (eingebaut) | Vulkan forward ↔ deferred | 2,37 | 30 |
| dieselbe | GL deferred ↔ Vulkan deferred / D3D12 deferred ↔ Vulkan deferred | 0,058 / 0,033 | 18 / 14 |
| dieselbe, G-Buffer-Ansicht 1..4 | GL ↔ Vulkan | 0,000 / 0,013 / 0,210 / 0,629 | 1 / 4 / 4 / 4 |
| `INSTANCETEST` (eingebaut, instanziert) | Vulkan forward ↔ deferred / GL deferred ↔ Vulkan deferred | 1,32 / 0,004 | 30 / 9 |
| `SSRTEST` + `MATERIALTEST=matte` (Kugel auf dem Boden), TOD 0,26 | **SSAO** an ↔ aus, deferred / forward, Band | 0,115 / 0,120 | 30 / 30 |
| `DECALTEST` | Vulkan forward ↔ deferred / GL deferred ↔ Vulkan deferred | 1,00 / 0,001 | 20 / 1 |
| `ROPETEST` (Seile eingebaut, Spur Graph + transparent) | Vulkan forward ↔ deferred / GL deferred ↔ Vulkan deferred | 0,627 / 0,004 | 73 / 18 |
| `WATERTEST` | Vulkan forward ↔ deferred | 0,000 | 0 |

Was die Tabelle zeigt:
- Vulkan deferred trifft D3D12 deferred (0,007–0,05) und GL deferred, bei Graph-Materialien,
  Atlas-Schatten, Landscape und bei eingebauten Materialien. Die G-Buffer-Ansichten gegen GL
  ergeben dieselben Zahlen wie D3D12 ↔ GL in 10.10.
- Die Gegenproben zeigen:
  - Der clustered Resolve lichtet alle 16 Lichter.
  - GI wirkt im Resolve.
  - SSAO wirkt im Resolve so stark wie forward.
  - Die Ansicht 1 ersetzt die beleuchtete Kugel, der Resolve läuft also wirklich.
- Bei `ROPETEST` stammt das Maximum von den zwei Seilen. Sie haben Materialien ohne Graph, sind
  also eingebaut (10.6). Die transparente Graph-Spur ist in beiden Pfaden gleich, und gegen GL
  deferred liegen die Seile bei 0,004.
- `WATERTEST` sieht forward und deferred gleich aus. Gegen GL liegt es in beiden Pfaden bei 1,99
  (GL forward ↔ Vulkan forward 1,99); das ist vorbestehend und nicht Teil des Themas.

**Spielpfad.** Das Depthy-Spiel aus Thema 130 (Export aus `C:\hw150\game12`, darin die
Binärdateien und `Shaders/` dieses Zweigs) läuft mit `GameBackend=Vulkan` und Validation an,
aufgenommen per `PrintWindow` über `docs/spielpfad-postfx-run-game.ps1`.
- Mit `RenderPath=1` loggt es `swapchain post chain active (1600x900)` und dann
  `deferred frame (1600x900, clustered resolve, G-buffer view 0)`. Mit `RenderPath=0` erscheint
  keine Deferred-Zeile. In beiden Läufen gibt es 0 Validation-Meldungen.
- Das Bild zeigt Geometrie, CSM-Schatten und Himmel vollständig. Die eingebauten Würfel tragen
  den heLitP-Look, forward ↔ deferred 16,8/255 unter dem Himmel (10.6).
- **Resize** (`-Mode resize`, acht Schritte zwischen 960×540 und 1800×1000, schnelles und
  langsames Ziehen, Minimieren und Wiederherstellen): Jedes Bild in 1600×900 ist zum Pixel gleich
  einem frischen Start in 1600×900 (0 von 1 080 000 Pixeln unter dem Himmel). Der G-Buffer wird
  nach jedem Resize sauber neu gebaut.
- Beim Minimieren meldet die Validation elf Fehler zu Ausdehnung 0×0 (`vkCreateSwapchainKHR`,
  `vkCreateImage`, `vkCreateFramebuffer`, `renderArea`). Dieselben elf kommen im Forward-Lauf
  derselben Reihe. Das ist vorbestehend (Swapchain-Neuaufbau beim Minimieren) und nicht Teil
  dieses Schritts.

**Bildtest-Skript (`scripts/he_vk_imagetests.py`).**
- Paare können jetzt einen **Höchstwert** tragen (viertes Element, 10.8 Schritt 4: „Deferred ≈
  Forward“).
- Neue Fälle:
  - `deferred`: Graph-Kugel forward ↔ deferred höchstens 1,0; deferred ↔ G-Buffer-Ansicht 1
    mindestens 2,0; Logzeilen `deferred path ready`/`deferred frame`.
  - `deferred_clustered`: 16 Lichter forward ↔ deferred höchstens 1,0; 8-Licht-Resolve ↔
    clustered mindestens 1,5.
- Beide laufen im CI-Job ohne Allowlist (`ALLOWED_VALIDATION` ist leer).
- Lokal gegen die RTX 4070 (das Skript verlangt sonst `llvmpipe`, das ist der einzige rote Punkt
  dort): 0,0063 / 5,16 bzw. 0,0091 / 2,80, 0 Validation-Meldungen. `builtin` und `landscape`
  sind nach der Umstellung Skinned-vor-Transparenz unverändert (Proben 192,71,83 usw.).
- **Ein lavapipe-Lauf steht aus.** Er kommt mit dem CI-Lauf dieses Schritts.

**he_tests.** „Vulkan: the deferred resolve's set layout covers both resolve variants, and the
G-buffer tails fit the material layout (Thema 150)“, 524 Zusicherungen:
- Beide Resolve-Varianten, reflektiert per SPIRV-Cross, liegen ganz in `kResolveBindings`: Set,
  Binding, Art und Fragment-Stage.
- Sie lesen 19–22, 23 und 0 wirklich. Clustered fügt genau 24–26 hinzu.
- Gegenproben:
  - Ohne die GB3-Zeile bleibt genau „0/22:1“ ungedeckt.
  - Gegen das Material-Layout fehlen genau 19–23.
- Die G-Buffer-Tails aller Surface-Knoten-Graphen passen in das Material-Layout (14 inklusive).
- Gesamtlauf `he_tests` (Release, nach Merge und Schritt 4): 4482 von 4484 Fällen grün. Die zwei
  roten sind die bekannten Zwischenablage-Fälle in `test_inspector_ui` (7 Zusicherungen),
  vorbestehend, dieselben wie vor Schritt 4 direkt nach dem Merge.

**D3D11/D3D12 nach dem Merge.** Der Merge hat D3D11- und D3D12-Renderer berührt (zwei Konflikte,
rund 330 bzw. 430 Zeilen aus release). Beide Deferred-Pfade sind danach zur Laufzeit gelaufen:
- D3D11 `MATERIALTEST=matte`: forward ↔ deferred 0,045/255 auf der Kugel, `deferred frame` im
  Log. GL deferred ↔ D3D11 deferred liegt jetzt bei 0,047 / 0,271 (Band / Kugel). In 10.9 waren
  es 2,49; die release-Änderungen bringen D3D11 näher an GL.
- `MANYLIGHTS=16`: D3D11 deferred ↔ D3D12 deferred zum Byte gleich.
- Die D3D12-Läufe der Tabelle oben: Debug-Layer an, 0 `[ERROR]`-Zeilen, `deferred frame` in
  jedem.

**Abweichungen zu Metal/GL (Vulkan).**
- *Eingebaute Materialien* sehen in Vulkan deferred anders aus als in Vulkan forward:
  - 2,37/255 in `SHADOWINSTTEST`, 16,8/255 im Depthy-Spiel.
  - Vulkans `scene.frag` ist die am stärksten reduzierte Kopie (ohne SkyEnv, `uAmbient`, Wetter;
    10.6). Deferred trifft GL (0,058) und D3D12 (0,033).
- *Vulkan forward bei Atlas-Schatten* weicht von GL ab (15,8/255), dasselbe Bild wie D3D11/D3D12
  forward. Der Deferred-Pfad trifft GL.
- Vulkan füllt wie D3D `lit.weather`, `lit.specAA` und `lit.viewMode` nicht. Nässe/Schnee und
  Specular-AA fehlen also auch im Resolve.
- *SSR* läuft im Deferred-Frame nicht. *Decals* bleiben selbst beleuchtet über dem Resolve, mit
  dem Tiefen-Vorpass wie forward. *SSAO* liest das Prepass-Ergebnis. Das alles ist Schritt 5;
  GB3 liegt für Decals/SSR/SSAO schon bereit. **Erledigt in Schritt 5:** SSR und Decals in GB0
  (der Decal sampelt GB3, der Tiefen-Vorpass entfällt im Deferred-Frame), SSAO bleibt beim
  Prepass, siehe 10.12.
- *Kosten:* zwei zusätzliche Pass-Wechsel je Deferred-Frame (Himmel-Pass endet, G-Buffer-Pass)
  und 24 Byte/px G-Buffer (GB3 eingeschlossen). Nicht gemessen; ein Perf-Vergleich gehört zu
  Schritt 6.
- *Nicht geprüft:* MoltenVK (kein Mac an diesem Gerät; `clang -fsyntax-only` entfällt, der Code
  ist hier mit MSVC voll übersetzt, Linux/Mac übersetzt die CI), AMD/Intel, lavapipe (CI).
- *Kein Tile/Single-Pass* (10.6). Subpass-Input-Attachments wären auf Vulkan möglich, sie lohnen
  sich nur auf TBDR.

### 10.12 Schritt 5 umgesetzt: Zusatzpässe im Deferred-Frame (Stand 2026-10-08)

> Zweig wie oben, Code-Commits `dfd1e7c2` (SSR) und `4c8c37ea` (Decals). Gemessen auf NN-WS03
> (RTX 4070, Release, privater Deploy `C:\hw150`, eigenes APPDATA je Lauf, **D3D12-Debug-Layer
> und Vulkan-Validation an** über `HE_GPU_DEBUG=1`; D3D11 hat keinen Debug-Layer, 10.9). Skripte:
> `scripts/deferred-witness/run_s5.ps1` (Editor-Aufnahmen), `ana_s5.py` (Auswertung),
> `game_s5.ps1` (exportiertes Spiel). „Echte Hardware“ heißt auch hier nur diese eine NVIDIA-Karte;
> lavapipe und WARP haben die neuen Wege noch nicht gesehen (Schritt 6).

**Was jetzt im Deferred-Frame läuft, je Pass.**

| Pass | D3D11 / D3D12 / Vulkan im Deferred-Frame | Bezug zu Metal/GL |
|---|---|---|
| SSR | Forward-Trace (Refl-Prepass, Trace, Blur gegen das Vorframe-HDR), Composite in `heLitP` im Resolve | GL deferred hat keins, Metal nur im Tile-Pfad (dort ohne Lag aus dem G-Buffer) |
| Decals | `decalFragmentSampled` in GB0 vor dem Resolve, der Resolve lichtet den Decal | wie GL deferred (gemessen gleich) |
| AO (SSAO/HBAO/GTAO) | Prepass-Ergebnis im Resolve, alle drei Methoden | GL/Metal rechnen es aus der G-Buffer-Tiefe; das Bild ist dasselbe, nur der Weg nicht |
| GI (DDGI, Masken) | im Resolve (seit Schritt 2–4) | wie GL/Metal |
| TAA / SMAA / FXAA, HDR, Tonemap, Bloom | Post-Kette unverändert hinter dem Resolve | wie GL/Metal |
| Spielpfad | das exportierte Spiel fährt über `SetSwapchainPostProcessing` den Viewport-Frame und damit dieselbe Kette | wie GL/Metal |

Kein Pass fehlt im Deferred-Frame. Ein eigenes Capability-Flag pro Pass war deshalb nicht nötig:
`supportsDeferredRendering` und `supportsScreenSpaceReflections` sagen auf allen drei Backends die
Wahrheit (die Kommentare in `IRenderer.h` sind nachgezogen). Bis zu diesem Schritt schaltete der
Deferred-Frame SSR still ab, obwohl der Schalter im Editor an war; das ist behoben.

**SSR (`dfd1e7c2`).**
- Der Deferred-Frame fährt jetzt denselben Forward-Trace wie ein Forward-Frame. Der Resolve
  komponiert ihn über die `heSSRFwd`-Stufe von `heLitP`, die auf Vulkan auch die Graph-Materialien
  forward benutzen.
- Der Gate (`ssr.x/y/z`) steht **nur** im Resolve-eigenen `HeLighting`. `frameMatLight` bleibt auf
  D3D bei `ssr.x = 0`, weil Forward-Replay und transparente Graph-Materialien t31 nicht gebunden
  haben (Plan C5, `docs/ssr-cross-backend-plan.md`).
- D3D11 bindet für den Resolve t31 + s8 (linear clamp) und löst t31 danach wieder. D3D12 legt das
  Trace-Ergebnis in Slot 14 (t31) der Resolve-Tabelle, s8 ist der statische Sampler der Signatur.
  Vulkan zeigt Binding 31 des Resolve-Sets auf das Trace-Ergebnis.
- **Abweichung vom Bauplan (10.8, §9):** kein Trace aus GB1 + Tiefe und kein `ssrComposite` nach dem
  Resolve. Folgen:
  - Ein Frame Lag in der Strahlungsquelle, wie forward.
  - Der Refl-Prepass rastert die opake Geometrie im Deferred-Frame ein zweites Mal.
  - `ssrComposite` bleibt für HLSL ungepinnt (10.7 #5), weil ihn niemand braucht.
  - Den lag-freien Weg aus dem G-Buffer hat weiterhin nur der Metal-Tile-Pfad.
- Auf D3D bekommen damit Graph-Materialien im Deferred-Frame SSR, forward aber nicht (C5). Ein
  Graph-Spiegel sieht auf D3D also deferred anders aus als forward. Das ist gewollt, nicht als Fehler
  zu werten.

| Szene (`SSRTEST` + Chrom-Graph-Kugel, TOD 0,45) | Vergleich | mittlere Abw. | max | > 8 |
|---|---|--:|--:|--:|
| Band | D3D11 deferred SSR an ↔ aus | 2,810 | 156 | 8,2 % |
| Band | D3D12 deferred SSR an ↔ aus | 2,810 | 156 | 8,2 % |
| Band | Vulkan deferred SSR an ↔ aus | 2,840 | 156 | 8,3 % |
| Band | D3D11 deferred ↔ D3D12 deferred | 0,000 | 0 | 0 |
| Band | D3D12 deferred ↔ Vulkan deferred | 0,051 | 76 | 0,27 % |
| Band | D3D12 forward ↔ Vulkan forward (Referenz) | 0,655 | 67 | 2,8 % |
| Band, Qualität 2 (History, Kamera dreht) | D3D12 deferred ↔ Vulkan deferred / forward (Referenz) | 0,810 / 0,868 | 64 / 68 | |
| Kugel (Graph), SSR aus | forward ↔ deferred, D3D12 und Vulkan | 0,433 | 3 | |
| Kugel (Graph), SSR an | Vulkan forward ↔ deferred | 3,33 | 46 | |

- Die Kugel spiegelt den eingebauten Boden, und der sieht in beiden Pfaden absichtlich verschieden
  aus (10.6). Daher die 3,33 bei SSR an gegenüber 0,43 ohne.
- Im Bild spiegeln Boden und Wand Kugel und Würfel; die untere Kugelhälfte spiegelt den Boden.
- 0 Fehler in allen SSR-Läufen. Die eine Vulkan-Warnung „Vertex attribute at location 2 not
  consumed“ kommt aus der Refl-Prepass-Pipeline und erscheint im Forward-Lauf genauso.

**Decals in GB0 (`4c8c37ea`).**
- D3D11 und D3D12 bekommen eine zweite Decal-Variante mit `decalFragmentSampled` (D3D11 ein zweiter
  Pixel-Shader, D3D12 eine PSO gegen `R8G8B8A8_UNORM_SRGB`). Sie läuft direkt nach dem
  G-Buffer-Pass auf GB0 allein. Die Tiefe kommt dabei wie im Forward-Decal vom Output-Merger.
- Vulkan bekommt einen eigenen Render-Pass `m_gbDecalRP`: nur GB0, `LOAD`, `SHADER_READ_ONLY` rein
  und raus, eigene Abhängigkeiten. Dazu kommen Framebuffer und Pipeline. Der Decal sampelt **GB3**
  statt eines Tiefen-Vorpasses.
- Die Deferred-Entscheidung fällt auf Vulkan jetzt am Anfang von `DrawViewportFrame`, damit ein
  Deferred-Frame den Decal-Tiefen-Vorpass überspringt. Auch der Forward-Rückfall (unten) liest dann
  GB3.
- Blend wie GL: `SrcAlpha/InvSrcAlpha`, nur RGB (GB0.a = Metallic bleibt), auf dem sRGB-Ziel linear.
- Jede GB-Variante ist optional. Baut sie nicht, zeichnet der Deferred-Frame den selbst beleuchteten
  Forward-Decal über dem Resolve, und das Log schreibt eine WARN-Zeile.
- **Witness:** `DECALTEST`, darüber die Graph-Kugel bei (2,5; 4; −6,5), sodass ihr CSM-Schatten über
  dem Decal liegt. Kamera CAMY 13 / CAMZ −1 / PITCH −65. Der alte `DECALTEST` allein war für den
  Unterschied blind: selbst beleuchtet und im Resolve beleuchtet lagen dort 0,001/255 auseinander
  (10.11).

| Aufnahme | Decal im Schatten | Decal in der Sonne | Schatten/Sonne |
|---|---|---|--:|
| GL deferred | (117, 67, 89) | (244, 184, 189) | 0,44 |
| D3D11 / D3D12 / Vulkan **deferred** | (117, 67, 89) | (244, 184, 189) | 0,44 |
| D3D11 / D3D12 / Vulkan forward (selbst beleuchtet) | (219, 140, 151) | (243, 168, 174) | 0,87 |

- Band: GL deferred ↔ D3D11/D3D12 deferred 0,001/255 (max 1), ↔ Vulkan deferred 0,001 (ein Pixel
  max 93, 0,001 %). D3D11 ↔ D3D12 deferred 0,000.
- Der Forward-Decal ignoriert den Schatten (Verhältnis 0,87). Im Deferred-Frame dunkelt er darin ab
  wie auf GL. 0 Fehler in allen Läufen.

**AO-Methoden im Deferred-Frame** (`SSRTEST`-Boden + matte Graph-Kugel, TOD 0,26, SSR aus,
`SSAOMethod` über die Editor-Config). Wirkung = AO an ↔ aus im Band:

| Methode | GL fwd / def | D3D11 fwd / def | Vulkan fwd / def | GL def ↔ D3D11 def / ↔ Vulkan def |
|---|--:|--:|--:|--:|
| SSAO (0) | 0,09 / 0,07 | 0,15 / 0,13 | 0,12 / 0,12 | 0,12 / 0,09 |
| HBAO (1) | 8,0 / 8,0 | 18,1 / 17,2 | 9,1 / 9,1 | 9,2 / 1,3 |
| GTAO (2) | 16,4 / 16,4 | 58,3 / 46,1 | 16,9 / 16,1 | 30,0 / 0,44 |

- Alle drei Methoden wirken im Deferred-Frame so stark wie forward. Vulkan deferred trifft GL
  deferred.
- D3D12 deferred ↔ D3D11 deferred: 0,000/255, max 1, für alle drei Methoden.
- **Vorbestehend, nicht aus diesem Schritt:** HBAO und GTAO sind auf D3D11/D3D12 schon **forward**
  etwa doppelt (HBAO) bzw. dreieinhalbmal (GTAO) so stark wie auf GL und Vulkan. Der Deferred-Pfad
  reicht das nur durch. Ursache nicht untersucht; sie liegt im D3D-AO-Kern, nicht im Deferred-Pfad.
  Die Szene oben reproduziert es in Sekunden.

**Post-Kette** (Graph-Kugel, Bloom an, SMAA):
- Forward ↔ deferred: 0,009/255 auf allen drei (max 19/19/28).
- Bloom+SMAA ↔ ohne, deferred (Kontrolle, dass die Kette läuft): 0,126 / 0,126 / 0,137 (max 79–81).
- GL deferred ↔ D3D11/D3D12 deferred 0,695, ↔ Vulkan deferred 0,671 (max 35).
- TAA lief deferred schon in Schritt 2–4 (Tabellen dort).

**Spielpfad** (Depthy aus Thema 130):
- Aufbau: Kopie von `C:\hw150\game12` als `game15`, darüber `deploy\Game\*`, dann `HorizonRendering.dll`
  aus dem Build-Baum und die `.spv` aus `deploy\Editor\Shaders`. Gestartet über
  `docs/spielpfad-postfx-run-game.ps1`.
- Config `RenderPath=1`, `SSREnabled`, `BloomEnabled`, `AntiAliasing=2`. Aufnahme per `PrintWindow`,
  1600×900 physisch.
- Ergebnisse:
  - Jeder Deferred-Lauf loggt `swapchain post chain active` und `deferred frame (1600x900, clustered
    resolve, …)`. Mit SSR kommt im selben Frame die SSR-Pipeline-Zeile dazu, die es im
    Deferred-Frame vorher nicht geben konnte.
  - 0 Fehler, D3D12-Debug-Layer und Vulkan-Validation an.
  - Unterhalb des Himmels SSR an ↔ aus, deferred: D3D11 0,767 / D3D12 0,767 / Vulkan 0,764 (max 76,
    2,5 % > 8). Reflexionen auf Würfelflächen und Boden.
  - D3D11 deferred ↔ D3D12 deferred 0,000, ↔ Vulkan 0,143.
  - Forward ↔ deferred 16,9 (eingebaute Würfel, 10.6, wie 10.11).
- Zwei Fallen beim Aufbau:
  - Mit der alten `HorizonCore.dll` aus `game12` neben der neuen `HorizonRendering.dll` startet das
    Spiel ohne Fenster und ohne Log (der Loader-Fehler bleibt unsichtbar). `deploy\Game\*` muss mit
    über die Kopie.
  - Im Build-Baum dieses Schritts fehlte `gbuffer.frag.spv` in `deploy\Game\Shaders`, weil
    HorizonGame seit Schritt 4 nicht neu gelinkt war. Dann bleibt Vulkan mit einer WARN-Zeile („the
    deferred path stays off“) forward. Das ist **keine Lücke im Export**:
    - Der POST_BUILD von HorizonGame kopiert das ganze Build-Verzeichnis `Shaders/`.
    - Nach einem Relink von HorizonGame und HorizonEditor liegt die Datei md5-gleich in
      `deploy\Game\Shaders` und `deploy\Editor\Game\Shaders`.
    - Der Exporter kopiert `Game/Shaders` mit (`04de39bd` ist im Zweig).

**Was offen bleibt.**
- **SSAO aus der G-Buffer-Tiefe (P5)** ist nicht portiert. Das AO-Bild ist dasselbe (Tabelle oben).
  Der Unterschied liegt nur in den Kosten: Der Prepass rastert die opake Geometrie, solange SSAO
  oder SSR an ist, ein zweites Mal. Mit dem SSR-Weg dieses Schritts spart der Umbau erst etwas, wenn
  auch SSR aus dem G-Buffer trace. Beides gehört zusammen, als eigenes Perf-Thema nach Schritt 6.
- **lavapipe/WARP:** Die neuen Wege (SSR im Resolve, GB0-Decals, `m_gbDecalRP`) haben nur die RTX
  gesehen. Fälle dafür gehören in Schritt 6.
- **Ungeprüft (gelesen, nicht gelaufen):**
  - der Forward-Decal-Rückfall im Deferred-Frame (er greift nur, wenn die GB-Variante nicht baut;
    auf Vulkan liest er jetzt GB3 statt des Vorpasses);
  - ein texturierter Decal in GB0 (der Zeuge ist untexturiert);
  - SSR zusammen mit TAA im Deferred-Frame.
- Nicht geprüft: AMD/Intel, MoltenVK.
