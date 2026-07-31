# Deferred Renderer — Umsetzungsplan (als wählbarer Render-Pfad, Metal zuerst)

> Stand: 2026-07-31 · Ziel: Neben dem bestehenden Forward-Renderer ein **Deferred**-Pfad, der
> im Editor und im Spiel umschaltbar ist (`Renderer ▸ Render Path: Forward | Deferred`).
> Metal zuerst (wie GI/SSR), Architektur so, dass die Shading-Mathematik **nicht** ein siebtes
> Mal kopiert wird.

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
