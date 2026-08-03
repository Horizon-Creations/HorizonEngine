# Screen Space Reflections (SSR) — Umsetzungsplan (Metal zuerst)

> Stand: 2026-07-31 (rev. 2 — auf `deferred-renderer-plan.md` abgestimmt) · Ziel: Metallische und
> glatte Materialien spiegeln die tatsächliche Szene, nicht nur die prozedurale Sky-Cubemap.
> Metal-only in v1 (wie GI), Architektur so gebaut, dass GL/D3D/Vulkan später ohne Umbau der
> Shading-Mathematik nachziehen können.

> **Abhängigkeit:** SSR hat **einen** Trace-Pass, aber **zwei** Composite-Wege — je nach
> Render-Pfad (siehe `docs/deferred-renderer-plan.md`). Empfohlene Reihenfolge: erst dessen
> **P0–P2** (G-Buffer + Resolve), dann SSR. Dann wird der Composite einmal geschrieben statt
> zweimal, und die Deferred-Variante ist zugleich die qualitativ bessere (kein Frame-Lag).
> Wer SSR früher will, baut Phase 1–3 unten im Forward-Pfad und ergänzt später 4.5.

---

## 0. Warum überhaupt

Die Engine hat als einzige Reflexionsquelle die prozedurale Sky-Cubemap `heSkyEnv`
(`MaterialShaderLibrary.cpp:477-483`, Metal-Pendant `MetalRenderer.mm:547-554`):

```glsl
vec3 Rrough = normalize(mix(reflect(-V, n), n, rough));
ambSpec     = texture(heSkyEnv, Rrough).rgb * specColor * (1.0 - 0.6 * rough);
```

Konsequenz: Ein Material mit `metallic = 1` hat per PBR-Definition **keinen Diffusanteil**
(`diffuseColor = baseColor * (1 - metal)`), also besteht sein gesamtes Erscheinungsbild aus
`ambSpec` + einem schmalen Blinn-Phong-Glanzlicht pro Lichtquelle. Es gibt weder SSR noch
Reflection-Probes → Metall spiegelt weder Boden noch Nachbarobjekte und wirkt „tot". Genau
dieser Fall ist in `ShadowValidation` aufgeschlagen (dunkle Kugel/Würfel neben heller Ebene).

SSR schließt exakt diese Lücke: `ambSpec` bekommt dort, wo der Screenspace einen Treffer
liefert, die **echte Szenenfarbe** statt der Himmelsfarbe.

---

## 1. Ausgangslage im Code (verifiziert)

| Baustein | Ort | Zustand |
|---|---|---|
| HDR-Szenenziel | `MetalRenderer.mm:6482` `EnsureHDRTarget`, `m_hdrColor`, `kSceneColorFormat = RGBA16Float` (`:164`) | vorhanden |
| View-Space-Position-Prepass | `kSSAOMSL` `ssaoPosVertex/ssaoPosFragment` (`:887 ff.`) → `m_ssaoPosTex` (RGBA16F, `xyz` = View-Pos, `a = 1` bei Geometrie) | vorhanden, **aber an SSAO gekoppelt** |
| Normalen | in `ssaoFragment` aus Positions-Ableitungen rekonstruiert (`cross(ddx, ddy)`) | wiederverwendbar |
| Pass-Reihenfolge | `MetalRenderer.mm:8381`: SSAO läuft **vor** dem Scene-Pass, halbe Auflösung, gated durch `m_ssaoEnabled && !giReplacesAO` | Muster für SSR |
| Screenspace-Faktor im Forward-Shading | `heAO` / `heGIShadow` werden per `gl_FragCoord` im Shading-Pass gesampelt | genau das Muster, das SSR braucht |
| Settings-Pfad | `IRenderer::SSAOSettings` (`IRenderer.h:168`) / `GISettings` (`:187`), `Capabilities` (`:53`), Editor-Persistenz `EditorApplication.cpp:669/1175/2945`, UI `EditorSettingsPanel.cpp:145` | 1:1 kopierbar |
| Profiler | `flushPass("…")` / `ftBeginMulti` / `ftAttachPass` | Pass eintragen |

---

## 2. Architekturentscheidung

Die Engine ist **Forward-Renderer**: Beleuchtung passiert im Geometrie-Fragment-Shader
(`fragmentMain` bzw. `heLitP`). Damit gibt es drei Wege, SSR zu integrieren:

**Option A — Trace vor dem Scene-Pass gegen die Farbe des Vorframes, Sampling im Shading-Pass**
*Gewählt.*
- Prepass liefert View-Position (existiert bereits), SSR-Pass marschiert im Screenspace und
  liest die Farbe aus einer Kopie von `m_hdrColor` des **letzten** Frames.
- Ergebnis landet in einer RGBA16F-Textur (`rgb` = Radiance, `a` = Confidence).
- Shading-Pass ersetzt damit `ambSpec` — exakt dieselbe Mechanik wie `heAO`/`heGIShadow`.
- ✅ Keine Änderung an Pipeline-Attachments, kein G-Buffer, **exakte** per-Pixel
  `specColor`/`roughness` (auch für Graph-Materialien mit berechnetem Metallic).
- ✅ Kein Doppelzählen: SSR *ersetzt* den Cubemap-Term, addiert nicht dazu.
- ⚠️ Ein Frame Verzögerung → Ghosting bei schneller Kamerabewegung (Gegenmittel: Reprojektion
  mit `prevViewProj`, siehe Phase 4; die Engine hält für die Low-Res-Wolken bereits ein
  `m_prepassViewProj`).

**Option B — Trace nach dem Lighting, Composite als eigener Fullscreen-Pass**
*Gewählt für den **Deferred**-Pfad.* Braucht einen G-Buffer mit `specColor`/`roughness`/Normale —
im Forward-Pfad wäre das ein zweites Color-Attachment an **jeder** Scene-Pipeline plus eine zweite
Ausgabe in **jedem generierten** Material-Fragment, also unverhältnismäßig. Sobald der
Deferred-Pfad existiert, **liegt dieser G-Buffer aber ohnehin da** — dann ist Option B strikt
besser als A: aktuelle Frame-Farbe statt Vorframe, kein Lag, kein Sampler-Slot nötig. Details in
4.5.

**Option C — Reflexions-Probes statt SSR**
*Später, orthogonal.* Löst das Off-Screen-Problem (SSR kann nur spiegeln, was auf dem Schirm
ist), ist aber ein eigenes Feature. SSR ist die richtige erste Stufe; Probes können später als
Fallback dienen, wo SSR keine Confidence hat (heute: Sky-Cubemap).

---

## 3. Der Blocker — und seine Lösung: Metal-Sampler-Budget

> Gilt **nur für den Forward-Pfad**. Im Deferred-Pfad braucht das Material gar keinen
> SSR-Sampler (es schreibt nur Attribute), und der Resolve-/Reflexions-Pass hat das
> Sampler-Budget für sich allein. Der Umzug in P0 lohnt trotzdem: er spart in beiden Pfaden
> 6 redundante Bind-Calls pro Frame und schafft Luft für Reflection-Probes.

Metal erlaubt **16 Sampler pro Fragment-Stage** (Index 0–15). Laut Kommentar in
`MaterialShaderLibrary.cpp:759-772` ist die Material-Pipeline **am Limit**, und ein 17. Pin lässt
die gesamte Pipeline still fehlschlagen → jedes Graph-Material fällt unbemerkt auf Built-in-PBR
zurück. SSR braucht aber einen Sampler in **beiden** Pipelines (Built-in + Material), die sich
**einen** Encoder teilen.

Belegung heute:

| Slot | Built-in Scene | Material-Pipeline |
|---|---|---|
| 0 | Albedo (pro Draw) | `heTex0` (pro Draw) |
| 1–4 | CSM (1), SkyEnv (2), AO (3) | `heTexP0..3` (pro Draw) |
| 5 | GI-Sun-Maske | — |
| 6, 7 | DDGI Irradiance/Visibility | dieselben (geteilt) |
| 8 | GI-Local-Maske | — |
| 9, 10 | **GI-Sun/Local-Maske nochmal** | `heGIShadow`, `heGILocal` |
| 11 | CSM nochmal | `heCsm` |
| 12 | Local-Shadow-Atlas | derselbe (geteilt) |
| 13–15 | — | Landscape-Weightmap, SkyEnv, AO |

**Befund:** Slots 9 und 10 halten *dieselben Texturen* wie 5 und 8 — an allen drei Bind-Stellen
(`MetalRenderer.mm:7391/7397/7400/7402`, `:7592/7598/7601/7603`, `:8068/8074/8077/8079`) wird
`m_giShadowResult` doppelt und `m_giLocalMaskTex` doppelt gebunden. Der Grund für die Trennung
(Kommentar bei `MaterialShaderLibrary.cpp:788`) gilt nur für 1–4: **die** werden pro Draw von
Graph-Texturen überschrieben. 5 und 8 fasst kein Per-Draw-Bind an.

**Lösung:** Die Preamble-Pins `heGIShadow` (GLSL-Binding 10) und `heGILocal` (11) von MSL 9/10 auf
**MSL 5/8** umziehen — exakt das Muster, das die DDGI-Atlanten mit 6/7 schon nutzen. Damit sind
**Slot 9 und 10 frei**, 6 redundante Bind-Calls pro Frame fallen weg, und SSR bekommt
**Slot 9 in beiden Pipelines** (Slot 10 bleibt Reserve, z. B. für Reflection-Probes).

*Nebenbedingung GL:* Ein voll bestücktes Material bindet auf GL heute 13 Sampler; mit `heSSR`
werden es 14 — unter dem GL-Minimum von 16. Kein Handlungsbedarf, aber ab jetzt eng: der nächste
Sampler nach SSR braucht wieder eine Analyse.

---

## 4. Pass-Design

### 4.1 Prepass (`EncodeViewPositionPrepass`)

`EncodeSSAO` wird zerlegt: der Positions-Prepass (`m_ssaoPosTex`) wandert in eine eigene Funktion,
die läuft, wenn **SSAO oder SSR** aktiv ist. Targets werden umbenannt
(`m_viewPosTex` / `m_viewPosDepth`), SSAO- und SSR-Pass konsumieren sie.

Zusätzlich: der Prepass macht heute ein eigenes `extract → cull → sort` des kompletten Worlds
(`MetalRenderer.mm:6720-6733`). Das wird für zwei Konsumenten nur **einmal** ausgeführt (reiner
Perf-Gewinn, verhaltensneutral, weil deterministisch).

**Auflösung:** halbe Breite/Höhe (wie SSAO heute). SSR ist niederfrequent genug, das Upsampling
passiert bilinear beim Sampling im Shading-Pass.

### 4.2 SSR-Trace (`EncodeSSR`, neues `kSSRMSL`)

Fullscreen-Triangle, ein Draw, Ziel `m_ssrTex` (RGBA16F, halbe Auflösung).

Eingaben:
- `m_viewPosTex` (Sampler 0) — View-Position + Gültigkeit
- `m_ssrHistoryTex` (Sampler 1) — Kopie von `m_hdrColor` des Vorframes (RGBA16F, volle Auflösung)
- `SSRParams` (Buffer 0)

```metal
struct SSRParams {
    float4x4 proj;         // Kamera-Projektion (GL-Konvention, wie SSAOParams::proj)
    float4x4 reproj;       // prevViewProj * inverse(view)  — View-Space → Vorframe-UV
    float4   cfg;          // x = maxDistance, y = thickness, z = maxRoughness, w = intensity
    float4   cfg2;         // x = stepCount, y = binarySteps, z = edgeFade, w = frameIndex
};
```

Kern (Ablauf, nicht Endfassung):

```metal
float4 pv = viewPos.sample(s, uv);
if (pv.a < 0.5) return float4(0.0);              // Hintergrund → keine Reflexion
float3 P = pv.xyz;
float3 N = reconstructNormal(viewPos, uv);       // dieselbe Ableitungs-Logik wie ssaoFragment
float3 V = normalize(P);                         // Kamera im Ursprung (View-Space)
float3 R = reflect(V, N);

// Rückwärts zur Kamera zeigende Strahlen sind im Screenspace nicht auflösbar → ausblenden
float facing = saturate(R.z * -1.0);

float3 ray = P;  float3 step = R * (P_len * stepScale);
// 1) lineare Suche: Marschieren, bis der Strahl HINTER die gespeicherte Geometrie fällt
// 2) Verfeinerung: binäre Suche (5 Iterationen) auf den Schnittpunkt
// 3) Dickentest: |rayZ - sceneZ| < thickness  → echter Treffer, sonst verworfen
```

Confidence (`a`-Kanal), multiplikativ:
- Bildrand-Fade (`edgeFade`, glatt in den letzten ~10 % der UV)
- Strahllänge / `maxDistance`
- `facing` (Reflexion Richtung Kamera → unbrauchbar)
- Roughness-Fade gegen `maxRoughness` (v1: über ~0.6 kein SSR)
- kein Treffer → 0

Der Treffer wird über `reproj` in die **UV des Vorframes** projiziert (Kamerabewegung
kompensiert), dann aus `m_ssrHistoryTex` gesampelt und gegen Feedback-Explosionen geklemmt
(`min(radiance, clampMax)` — die History enthält bereits die eigene Spiegelung).

Jitter: Der Startversatz des Marsches wird mit Interleaved-Gradient-Noise über `frameIndex`
verrauscht (dieselbe `ssaoIgn`-Funktion), damit der Blur in 4.3 die Bänder auflöst.

### 4.3 Filter (`EncodeSSRResolve`)

- **v1:** separierbarer 5-Tap-Gauss → **UMGESETZT** als `kSSRBlurFS` (eigener kanonischer
  GLSL-Shader statt `kPostFXMSL`-Klon, confidence-gewichtet: Miss-Pixel mit `a = 0` tragen
  keine Farbe bei, schwärzen also keine Trefferränder; die geblurte Confidence feathert den
  Übergang zur Cubemap). Zwei Passes H/V über `m_ssrPingTex` zurück nach `m_ssrReflTex`,
  aktiv ab Quality Med; `HE_DUMP_SSRQUALITY=0` erzwingt den rohen Trace fürs A/B.
- **v2 (Phase 4):** zwei Mip-Stufen; das Shading lerpt anhand von `roughness` zwischen scharf
  und unscharf → glaubwürdige „glossy" statt nur „mirror"-Reflexionen.
- **v2 (Phase 4):** temporale Akkumulation mit Reprojektion, wie sie `EncodeGIShadowRays`
  bereits macht (History-Textur + Verwerfen bei Tiefen-/Normalen-Bruch).

### 4.4 Composite — Forward-Pfad (Shading-Pass)

Genau **eine** Zeile ändert sich in beiden Shadern — der Cubemap-Sample wird zum Fallback:

```glsl
// vorher:  ambSpec = texture(heSkyEnv, Rrough).rgb * specColor * (1.0 - 0.6 * rough);
// nachher:
vec3 envSpec = texture(heSkyEnv, Rrough).rgb;
if (heLight.ssr.x > 0.5) {
    vec4 r = texture(heSSR, gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0)));
    envSpec = mix(envSpec, r.rgb, r.a * heLight.ssr.y);   // y = Intensität
}
ambSpec = envSpec * specColor * (1.0 - 0.6 * rough);
```

Wichtig: **beide** GI-Zweige in `heLitP` (`giProbe.y > 0.5` und der Ambient-Zweig) benutzen
`ambSpec` — die Ersetzung muss vor der Verzweigung passieren, sonst spiegelt es nur mit GI aus.

Zu ändernde Kopien (die bekannten 6 Klone der Shading-Mathematik):
1. `MaterialShaderLibrary.cpp` — `kLightingPreamble` / `heLitP` (**alle Backends**, gated)
2. `MetalRenderer.mm` — `fragmentMain` (Built-in PBR)
3. `OpenGLRenderer.cpp` `kUnlitFS`, `D3D11/D3D12` HLSL, `shaders/scene.frag` — nur der
   `ssr.x = 0`-Pfad, d. h. **kein** Textur-Bind, Verhalten byte-identisch zu heute.

`shaders/scene.frag` trägt bereits ein Drift-Banner; der neue Absatz wird dort dokumentiert statt
portiert. `tests/test_culling.cpp` („GI kernels …") vergleicht nur den GI-Block — der bleibt
unberührt.

### 4.5 Composite — Deferred-Pfad (eigener Reflexions-Pass, **kein** Lag)

Im Deferred-Pfad wandert der **indirekte Spekularterm komplett aus dem Lighting-Resolve heraus**
in einen eigenen Pass. Ablauf:

```
G-Buffer ▸ Lighting-Resolve (Direktlicht + indirekt DIFFUS, ohne ambSpec)
         ▸ SSR-Trace  (gegen die JETZT fertige m_hdrColor + G-Buffer-Depth)
         ▸ Reflexions-Pass (additiv): envSpec = mix(skyEnv(Rrough), ssr.rgb, ssr.a * intensity);
                                      hdr += envSpec * specColor * (1 - 0.6*rough);
```

`specColor`, `roughness` und die Normale kommen exakt aus dem G-Buffer — kein Vorframe, keine
Reprojektion, kein Ghosting, keine History-Textur, kein zusätzlicher Sampler in der
Material-Pipeline. Dass `ambSpec` aus dem Resolve verschwindet, ist **kein** Sonderfall im
Shading-Code: `heLitP` bekommt dafür ein Flag in `heLight.ssr.w` (`1` = „Spekular-IBL überspringen,
ein späterer Pass liefert ihn nach"), das der Forward-Pfad nie setzt. Die Preamble bleibt also
weiterhin die einzige Shading-Quelle für beide Pfade.

Konsequenz für die Phasen: **P4 (Qualität) schrumpft im Deferred-Pfad** — temporale Akkumulation
und Reprojektion sind dort nur noch Rauschglättung, keine Lag-Kompensation mehr.

---

## 5. Daten / ABI

`MaterialShaderLibrary::Lighting` ist **append-only** (Header-Kommentar `:28-32`), also ans Ende:

```cpp
// SSR (append-only, v2.7): x = 1 wenn heSSR gebunden+gültig, y = Intensität,
// z = maxRoughness (Fade-Ende), w = reserviert
float ssr[4] = {};
```

Preamble bekommt `layout(set = 0, binding = 19) uniform sampler2D heSSR;`, Metal-Pin
`{ Stage::Fragment, 0, 19, 9 }`. Auf allen anderen Backends wird eine 1×1-Dummy-Textur gebunden
und `ssr.x = 0` gesetzt → toter Code, den glslang wegoptimiert, sobald der Zweig konstant ist.

Neu in `IRenderer.h`:

```cpp
struct SSRSettings {
    bool  enabled      = false;
    float intensity    = 1.0f;   // 0 … 1 Mischung gegen die Sky-Cubemap
    float maxRoughness = 0.6f;   // darüber kein SSR (Ausblendung)
    float maxDistance  = 30.0f;  // View-Space-Einheiten
    float thickness    = 0.5f;   // Dickenannahme des Tiefenpuffers
    int   quality      = 1;      // 0 Low (16 Steps) / 1 Med (32) / 2 High (64 + Mips)
};
virtual void SetSSRSettings(const SSRSettings&) {}
// Capabilities:
bool supportsScreenSpaceReflections = false;   // v1: nur Metal
```

---

## 6. Phasenplan

Jede Phase ist einzeln commit- und verifizierbar; nach jeder Phase läuft die Engine.
P0–P2 sind **pfadunabhängig**; ab P3 gabelt sich der Composite (4.4 Forward / 4.5 Deferred).
Existiert der Deferred-Pfad bereits, entfallen P3-Forward und der History-Blit aus P1 —
dann startet SSR direkt mit 4.5.

### P0 — Vorbereitung (kein sichtbarer Effekt)
1. `heGIShadow`/`heGILocal` von MSL 9/10 → 5/8 umpinnen (`MaterialShaderLibrary.cpp:788-795`),
   die 6 redundanten Binds an den drei Stellen entfernen, Slot-Tabelle im Kommentar fortschreiben.
2. Positions-Prepass aus `EncodeSSAO` herauslösen → `EncodeViewPositionPrepass`,
   `m_ssaoPosTex` → `m_viewPosTex`, gemeinsames extract/cull/sort.
3. `SSRSettings` + `SetSSRSettings` + `supportsScreenSpaceReflections` (überall No-op).
4. `Lighting::ssr` anhängen + `heSSR`-Binding in der Preamble (immer 0/Dummy).

**Verifikation:** Alle 810 Tests grün; `he_shot` A/B gegen einen Referenz-Shot der aktuellen Szene
→ Pixel-identisch (P0 darf **nichts** verändern).

### P1 — Trace-Pass, isoliert sichtbar
`kSSRMSL`, `m_ssrTex`, `EncodeSSR`, History-Kopie von `m_hdrColor` am Frame-Ende (Blit).
Debug-Ausgabe: `HE_DUMP_SSRDEBUG=1` rendert `m_ssrTex` direkt in den Backbuffer.

**Verifikation:** headless — Boden + Würfel, Kamera flach; die SSR-Textur muss das gespiegelte
Objekt als zusammenhängende Fläche zeigen, Confidence am Bildrand sauber ausblenden.

### P2 — Composite im Built-in-Shader
`fragmentMain` bekommt `heSSR` (Slot 9) + die `mix`-Zeile.

**Verifikation:** Witness-Szene `HE_DUMP_SSR=point|mirror` (analog `HE_DUMP_LOCALSHADOW`): spiegelnder
Boden (`metallic = 1`, `roughness = 0.05`) + farbige Objekte. A/B `SSR=0` vs `SSR=1`, Differenzbild.

### P3 — Composite in `heLitP` (Graph-Materialien, Forward-Pfad)
Dieselbe Zeile in der Preamble + Metal-Pin. Damit sieht ein Graph-Material identisch aus wie ein
Built-in daneben.

**Status: FORWARD-PFAD KOMPLETT (Metal), inkl. GI-Reflections.** Umsetzung weicht vom
v1-Entwurf ab (besser): der SSAO-Prepass rendert als **MRT** (View-Pos + Oct-Normale/Rough-0 +
NDC-Depth in `gbufferMain`-Konvention) — dadurch konsumieren der **unveränderte** SSR-Trace und
die **unveränderten** GI-Refl-Kernel im Forward-Pfad dieselben Eingaben wie im Deferred-Pfad.
Einziger Trace-Unterschied: `cfg2.z` = Farbe aus der **Vorframe-HDR-Kopie** (Blit nach dem
Scene-Pass), Hit via `prevViewProj` reprojiziert → 1 Frame Content-Lag (Option A). Composite in
`heLitP` (Kaskade Sky → `heGIReflFwd` → `heSSRFwd`, Bindings 31/32 → Metal-Slots 9/10) und
byte-analog in `fragmentMain` (SceneUniforms `reflCfg/reflCfg2`); Roughness-Fade per-Pixel im
Shader (der Prepass hat keine Materialdaten). Der P0-Slot-Umzug ist dabei umgesetzt: GI-Masken
der Material-Pipelines von 9/10 auf die geteilten Built-in-Slots 5/8, 6 redundante Binds weg.
Temporal (High) läuft auch forward (gleiches History-Paar); der Glossy-Wide-Lerp bleibt
deferred-only (kostete den letzten Sampler-Slot). D3D12/Vulkan: Bindings 31/32 folgen dem
Bestandsmuster von heSkyEnv/heAO (uniform-gated, Material-Layouts dort decken schon 13–18
nicht ab — bekannte Blind-Backend-Lücke).

**Verifikation:** `ShadowValidation` headless rendern — die metallische Kugel (`123Test.hasset`,
Metallic 1.0) muss die weiße Ebene spiegeln. Genau der Ausgangsfall dieses Plans.

### P3d — Composite im Deferred-Pfad (statt oder zusätzlich zu P3)
`ambSpec` aus dem Lighting-Resolve herauslösen (`heLight.ssr.w`), Reflexions-Pass nach 4.5,
SSR-Trace hinter den Resolve verschieben (History-Textur entfällt).

**Verifikation:** dieselbe `ShadowValidation`-Szene in beiden Pfaden — Deferred muss bei
statischer Kamera nahe an Forward liegen und bei **bewegter** Kamera sichtbar sauberer sein
(kein Nachziehen der Spiegelung).

### P4 — Qualität
Roughness-Mips + roughness-abhängiger Lerp, temporale Akkumulation mit Reprojektion und
Verwerfen bei Tiefenbruch, Fresnel-Gewichtung (`F_Schlick(NdV, specColor)`) statt flachem
`specColor`, Feedback-Clamp.

**Status:** Blur (4.3 v1) ✅; Glossy-Lerp ✅ (zweite breite Blur-Stufe statt Mips, Quality
High, Composite lerpt per G-Buffer-Roughness); **Trace-Qualität v2 ✅** — der alte
Forward-only-Facing-Gate (`smoothstep(0, 0.2, R·camFwd)`) tötete JEDE kamerazugewandte
Spiegelfläche (Wand spiegelt Nachbarobjekt nie); jetzt faden nur Fast-Rückwärts-Strahlen
(`smoothstep(-0.9, -0.4, ·)`), dazu weiche Thickness-Confidence statt binärem Treffer
(Speckle an dünnen/streifenden Features → Rampe), 6 Binärschritte, zweite 5-Tap-Iteration
ab Med (4 Half-Res-Draws); Witness `HE_DUMP_SSRTESTWALL=1`; Fresnel ✅ (roughness-aware Schlick
`F0 + (max(1-rough, F0) - F0)·(1-NdV)^5` in heLitP + SSR-Composite + Metal/GL/D3D11/D3D12-
Built-ins; `scene.frag` = dokumentierter Drift; Drift-Guard in `test_culling.cpp` prüft
heLitP↔Composite byte-identisch). **Temporale Akkumulation ✅ (Quality High):** der Trace
rendert MRT in ein History-Paar (geblendete Radiance + Empfänger-WorldPos), der Start-Jitter
rotiert per Frame (Golden Ratio) und ein adaptives EMA (0.85 statisch / 0.55 bei
Kamerabewegung, Kollaps bei Luminanz-Brüchen — GI-Refl-Schema) konvergiert die Samples gegen
den exakten Schnittpunkt. Blur-Politik pro Stufe: Low roh, Med 2× 5-Tap (Blur = Denoiser,
weich), High 1× 5-Tap + Temporal — spiegelglatte Flächen bleiben SCHARF statt verwaschen —
+ Wide-Stufe für den Glossy-Lerp. Feedback-Clamp entfällt: die History speist sich aus der
aktuellen Frame-Farbe (kein Selbst-Feedback wie im Forward-Entwurf).

**Verifikation:** Kamerafahrt (`HE_DUMP_GIROTATE`-Muster) → kein sichtbares Ghosting/Schweifen;
Profiler-Capture: SSR-Pass unter Budget (7.).

### P5 — Integration
`EditorConfig` (`SSREnabled/SSRIntensity/SSRMaxRoughness/SSRQuality`) lesen/schreiben/pushen
(`EditorApplication.cpp:669/1175/2945`), Settings-Panel-Block (`EditorSettingsPanel.cpp:145`
als Vorlage), Quick-Settings-Favorit `"ssr"`, `GameApplication`-GlobalState-Read (wie GI),
Capability-Gating (Toggle ausgegraut ≠ Metal), `docs/` + `MASTERPLAN.md` + Website-Roadmap.

### P6 — Andere Backends (später, optional)
GL 4.3 (Compute) bzw. GL 4.1 (Fullscreen-Fragment) und D3D/Vulkan. Die gesamte Shading-Seite ist
dann schon da — es fehlt nur der Trace-Pass + Bind pro Backend. macOS-GL kann den
Fullscreen-Weg gehen (kein Compute nötig).

---

## 7. Performance-Budget

Zielrahmen aus `docs/sky-weather-performance-plan.md`: 5 ms Gesamtframe.

| Pass | Auflösung | Budget |
|---|---|---|
| View-Pos-Prepass | halb | bereits bezahlt, wenn SSAO an; sonst ~0.3 ms (**einmalig geteilt**) |
| SSR-Trace | halb, 32 Steps + 5 Binary | ~0.4 ms Ziel |
| Blur/Resolve | halb, 2× 5-Tap | ~0.1 ms |
| History-Blit | voll, RGBA16F | ~0.1 ms |
| **Summe** | | **≤ 0.7 ms** bei aktivem SSAO |

Quality-Stufen: Low = 16 Steps ohne Mips, Med = 32 + Blur, High = 64 + Mips + temporale
Akkumulation. Messung mit dem vorhandenen Profiler („Detailed Capture", per-Pass-GPU ist auf
HW verifiziert).

---

## 8. Risiken & Gegenmaßnahmen

| Risiko | Gegenmaßnahme |
|---|---|
| **Ein Frame Lag** (Option A) | Reprojektion mit `prevViewProj`; Confidence-Fade bei großer Reprojektionsdistanz; erster Frame → `a = 0` (Cubemap) |
| **Metal-Sampler-Limit**: 17. Pin killt still jede Material-Pipeline | P0 schafft *vorher* zwei Slots; danach Slot-Tabelle im Kommentar Pflicht-Update. Zusätzlich: Startup-Log, wenn eine Material-Pipeline nicht baut (heute stiller Fallback!) |
| SSR spiegelt nur Sichtbares (Off-Screen, Rückseiten) | Confidence → Cubemap-Fallback; langfristig Reflection-Probes (Option C) |
| Doppelzählung mit dem GI-Spekular-Term | `ambSpec` wird *vor* der GI-Verzweigung ersetzt, beide Zweige nutzen denselben Wert |
| Selbst-Feedback (History enthält eigene Spiegelung) | Radiance-Clamp + Intensität < 1 |
| Drift der 6 Shading-Kopien | Der SSR-Block bekommt denselben SYNC-Kommentar wie der GI-Block; Erweiterung des String-Vergleichs in `test_culling.cpp` auf den Composite-Ausdruck |
| Prepass-Kosten, wenn SSAO aus | Prepass läuft nur, wenn SSAO **oder** SSR an; extract/cull/sort geteilt |

---

## 9. Offene Entscheidungen (brauchen deine Antwort)

0. **Reihenfolge:** erst `deferred-renderer-plan.md` P0–P2, dann SSR (Empfehlung — spart den
   Forward-Composite und liefert sofort die lag-freie Variante), oder SSR zuerst im Forward-Pfad,
   weil es schneller sichtbar ist?
1. **Default-Zustand:** SSR standardmäßig **aus** (wie GI) oder **an** für neue Projekte?
   Empfehlung: aus, bis P4 durch ist.
2. **Fresnel jetzt oder später:** Der korrekte Grazing-Angle-Boost verändert auch das Aussehen
   *ohne* SSR (alle bestehenden Szenen werden an flachen Winkeln spiegelnder). Empfehlung: P4,
   als eigener Commit mit Vorher/Nachher-Shots. → **Umgesetzt** (eigener Commit, A/B headless
   verifiziert; Vulkan/`scene.frag` bleibt flaches F0 = dokumentierter Drift).
3. **Reflection-Probes** als Folgefeature einplanen (löst Off-Screen), oder erst mal SSR + Sky?
4. **Material-Preview:** Soll die Vorschau eine feste Studio-Environment bekommen (siehe
   vorheriger Fix) — dann sähe Metall auch im Thumbnail richtig aus, unabhängig von SSR.
