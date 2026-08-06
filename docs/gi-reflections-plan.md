# GI-basierte Reflektionen (Ray-Traced Specular) — Plan, Metal-first

Stand: 2026-08-03. Basis-Audit des Ist-Zustands auf Branch `claude/gi-reflektionen-plan-0bf44e`.

> **Umsetzungsstand (2026-08-03): P0 + P1 + P2 implementiert und visuell verifiziert**
> (he_shot-A/B mit der `HE_DUMP_SSRTEST`-Spiegelszene: off = nur SkyEnv, GIRefl-on =
> rote Würfel-Spiegelung aus dem Trace, SSR+GIRefl = SSR gewinnt on-screen mit
> GIRefl-Backing an den Confidence-Rändern). Kernel `giReflRay` (kGIReflMSL),
> geteilter Composite in `EncodeSSRPasses`/`kSSRCompositeFS` (Kaskade SkyEnv →
> GIRefl → SSR), `GIReflectionSettings` + `supportsGIReflections` + komplettes
> Editor/Game-Plumbing + `HE_DUMP_GIREFL`. Bonus-Fix: GI-Instanz-Albedo löst jetzt
> die MaterialAsset-Farbe auf (`giInstanceAlbedo`) — vorher waren TLAS-Instanzfarben
> für Material-Objekte weiß (betraf auch den DDGI-Probe-Bounce-Tint).
> **Update (gleicher Tag): P3 + P4 + P5 ebenfalls implementiert und visuell verifiziert.**
> P3: Quality-Tiers 0/1/2 (raw / +Confidence-Blur / +Cone-Jitter mit In-Kernel-Temporal
> — History trägt Empfänger-Weltposition für Disocclusion-Reject, kamerareprojektiv —
> + Wide-Blur für den Glossy-Roughness-Lerp im Composite, `heGIReflRough` binding 30);
> Blur-Kette reuset die SSR-Blur-Pipeline verbatim. P4: echte interpolierte
> Vertex-Normalen am Hit via Tier-2-Argument-Buffer aus GPU-Adressen
> (`m_giMeshPtrBuf`, macOS 13+, useResource-Pflicht für die indirekt erreichten
> Buffer); Fallback -rayDir bleibt. P5: `giReflRaySw` in kGISWMSL (CPU-BVH,
> Closest-Hit mit Dreiecks-Index → geometrische Normale), Capability ohne
> HW-RT-Zwang, `HE_GI_FORCE_SW`-verifiziert. `HE_DUMP_GIREFLQUALITY` +
> `HE_DUMP_FRAMES` (Settle-Frames für Temporal-Konvergenz im Headless-Dump).
> Verify-Shots: Spiegelung farb-/shading-identisch zum Direktbild (P4), Q0-scharf
> vs. Q2-glossy bei Roughness 0.35 (P3), SW-Pfad bildgleich (P5).

## 1. Ziel & Scope

Indirekte **spekulare** Beleuchtung aus der bestehenden GI-Infrastruktur: Reflektionen, die
nicht auf Screen-Space-Information beschränkt sind (SSR-Lücken: Off-Screen-Geometrie,
Objekt-Rückseiten, Kamerarand). Der Ansatz ist derselbe wie bei DDGI-Diffuse: echte Strahlen
gegen die Szenen-Beschleunigungsstruktur, Hit-Shading über das vorhandene Probe-Feld
("one bounce + field"), Composite als eigener Fullscreen-Pass.

- **Metal-first** (HW-RT via `MTLAccelerationStructure`, SW-Compute-Fallback wie bei GI).
- Deferred-Pfad zuerst (dort liegen G-Buffer + SSR-Muster); Forward-Tail später/optional.
- GL/D3D/Vulkan explizit **nicht** in diesem Plan — aber Shading-Mathe so strukturieren,
  dass der Drift-Guard (`tests/test_culling.cpp:1114`) nicht verletzt wird (s. §7).

## 2. Vorhandene Anbindungspunkte (Ist-Zustand)

Alles Folgende existiert bereits und wird wiederverwendet:

### Beschleunigungsstrukturen (kein neuer Build nötig)
- **HW:** `EncodeGIAccelBuild()` (`MetalRenderer.mm:4672`) baut die TLAS **jede GI-Frame neu**,
  BLAS pro Mesh lazy/gecacht (`GpuMesh::blas`, `BuildBLAS` `:4534`). Läuft in `EncodeFrame`
  **vor** dem G-Buffer (`:10187`) — zum Zeitpunkt eines Reflection-Passes ist die TLAS fertig
  und residency-deklariert. Ein Reflection-Kernel muss nur `useResource:` auf TLAS +
  `m_giUniqueBlas` wiederholen (Muster: `:5347-5351`).
- **SW:** `EncodeGISwAccelBuild()` (`:4616`) + `HE::GiBvh` (`GiBvh.h`, 1521 Assertions in
  `test_gi_bvh.cpp`); Instanz-Scan im Kernel (`kGISWMSL`, Structs `GiNode/GiTri/GiInst`).
- **Filter:** nur `castsShadow`-Objekte; **skinned Meshes und Terrain fehlen** (bekannte
  GI-Lücke, gilt für Reflektionen identisch — s. Risiken §8).

### Hit-Daten (der eigentliche Engpass)
- Pro Hit ist heute nur `instanceColors[instanceId].rgb` verfügbar
  (`m_giInstanceColorBuffer`, `MetalRenderer.h:678`). Keine Normalen, UVs, Materialien.
- Der Probe-Kernel approximiert die Hit-Normale als `-rayDir` (`kGIProbeMSL:1562`) —
  dort als Follow-up markiert: Vertex-Buffer an den Kernel binden.
- `giSampleFieldIrradiance` (`kGIProbeMSL:1476`) sampelt das Probe-Feld an einer beliebigen
  Weltposition — genau das brauchen wir, um Hit-Punkte indirekt zu beleuchten (Multi-Bounce
  gratis, wie beim Probe-Update selbst).

### Shading-Einstiegspunkt (Double-Counting-Schalter existiert)
- `heLitP` (`MaterialShaderLibrary.cpp:438`) berechnet `ambSpec` (Sky-Cubemap × Fresnel) und
  hat bereits den Schalter **`if (heLight.ssr.w > 0.5) ambSpec = vec3(0);`** (`:495`) —
  „ein späterer Pass liefert das Env-Specular". Ein GI-Reflection-Pass übernimmt denselben
  Mechanismus; kein neues ABI-Feld nötig, solange er wie SSR additiv komponiert.
- `Lighting`-UBO ist append-only (`MaterialShaderLibrary.h:33-98`); freie Komponenten in
  `ssr[4]` bzw. ein neues Feld am Ende für Intensität/Flags.

### Pass-Schablone: SSR
- `EncodeSSRPasses()` (`MetalRenderer.mm:9555`) + `kSSRTraceFS`/`kSSRCompositeFS`/`kSSRBlurFS`
  (`MaterialShaderLibrary.cpp:1088-1304`): Half-Res-Trace (RGBA16F, rgb Radiance + a Confidence),
  Blur, additiver Fullscreen-Composite, der G-Buffer 0/1/2 + Depth liest, Weltpos via
  `invViewProj` rekonstruiert und die heLitP-Material-Splits SYNC-kommentiert nachspielt.
  **Der Composite ist 1:1 die Vorlage** — inklusive `mix(skyEnv(Rrough), refl.rgb, conf)`.
- SSR erzwingt bei Aktivierung gespeicherte (non-memoryless) G-Buffer (`m_gbStored` in
  `EnsureGBufferTargets`) — GI-Reflektionen brauchen exakt dasselbe.

### G-Buffer (liefert alles, was der Trace braucht)
- GB1: oct-Normale + Roughness + Specular; GB0: BaseColor+Metallic; `m_gbDepthLin` (R32F).
  Ray-Ursprung + Richtung (`reflect(-V,N)`) sind damit vollständig rekonstruierbar.
- **Keine Motion Vectors** → Temporal nur kamerabasiert (Muster: `giShadowTemporal` `:1237`).

### Settings/Toggles/Profiling
- `IRenderer::GISettings` + `SetGISettings`; EditorConfig-Zeilen in `EditorSettingsPanel.cpp:204`
  (SSR-Zeilen direkt daneben als Muster inkl. Quality 0/1/2); `flushPass("…")`-Profiler-Scopes;
  Headless-A/B via `scripts/he_shot.py` + `HE_DUMP_*`-Familie.

### Harte Randbedingung: Sampler-Cap
- Die Material-Pipeline steht bei **16/16 Samplern** (`MaterialShaderLibrary.cpp:1538`).
  Ein GI-Reflection-Ergebnis darf **nicht** als neuer Preamble-Sampler in die
  Material-Shader — es muss (wie SSR) als eigenständiger Composite-Pass laufen.

## 3. Architektur-Entscheidung

**Neuer Compute+Fullscreen-Pass „GIRefl", modelliert nach SSR, als Off-Screen-Fallback
UNTER SSR komponiert:**

```
EncodeFrame:
  … EncodeGIAccelBuild → EncodeGIShadowRays → EncodeGIProbeUpdate …
  EncodeGBuffer (+ Tile-Resolve)
  [neu] EncodeGIReflections(cmdBuf, w/2, h/2)     ← Compute-Trace gegen TLAS
  EncodeSSRPasses(...)                             ← Composite mischt: SSR > GIRefl > SkyEnv
```

Begründung:
- SSR bleibt die Qualitätsquelle, wo der Screen-Space-Hit existiert (schärfer, texturiert,
  volle Materialdetails). GI-Reflektionen füllen genau die SSR-Confidence-Lücken
  (Off-Screen, Ray-Marsch-Abbruch) — das ist die in `docs/ssr-plan.md` §2/§9.3 offen
  geparkte „Option C"-Frage, beantwortet mit RT statt Reflection-Probes.
- Composite-Kaskade in **einem** erweiterten `kSSRCompositeFS`:
  `env = skyEnv(Rrough)` → `env = mix(env, giRefl.rgb, giRefl.a)` → `env = mix(env, ssr.rgb, ssr.a·intensity)`.
  Läuft SSR nicht (aus/Non-Tile), übernimmt ein eigener GIRefl-Composite denselben Code mit
  `ssr.a = 0`. Der `heLight.ssr.w`-Schalter deckt beide Fälle ab.
- Hit-Shading = „one bounce": direkte Sonne am Hit (ein Shadow-Ray, wie `giShadowRay`)
  × Instanz-Albedo + `giSampleFieldIrradiance(hitPos, hitN)` × Albedo. Damit sind
  Reflektionen automatisch konsistent mit der DDGI-Diffuse-Beleuchtung (gleiches Feld,
  gleiche Intensität `giParams`), inkl. Mehrfach-Bounce über das Feld.

Bewusste v1-Vereinfachungen (dokumentieren wie beim GI-v1-Block in `MetalRenderer.h:652ff`):
- Nur Mirror-Ray pro Pixel (kein Importance-Sampling der GGX-Lobe); Glossy über
  Roughness-Blur des Ergebnisses (SSR-`m_ssrRoughTex`-Muster) + Roughness-Cutoff
  (`SSRMaxRoughness`-Analog), darüber weiter SkyEnv/Feld.
- Hit-Normale v1 = `-rayDir` (wie Probe-Kernel); echte Normalen in P4.
- Half-Res, 1 Ray/Pixel, temporale Akkumulation kameragebunden.

## 4. Phasenplan

### P0 — Scaffold + Toggle-Plumbing (klein)
- `GIReflSettings` (enabled, intensity, maxRoughness, quality 0/1/2) an `IRenderer`
  (append an `GISettings` oder eigenes Struct + Setter, SSR-Muster).
- EditorConfig-Keys `GIReflectionsEnabled/…` + `EditorSettingsPanel`-Zeilen (disabled +
  Tooltip wenn `!supportsGlobalIllumination`); GameApplication-GlobalState analog SSR.
- `m_giReflTex` (Half-Res RGBA16F: rgb Radiance, a Confidence/Blend), `Ensure/Destroy`-Paar,
  `flushPass("GIRefl")`, `HE_DUMP_GIREFL`-Headless-Dump.
- G-Buffer-Stored-Zwang: `m_gbStored |= giReflActive` in `EnsureGBufferTargets`.

### P1 — Trace-Kernel (HW-RT), Flat-Shading
- Neues MSL-String `kGIReflMSL`, `kernel giReflRay`: pro Half-Res-Pixel GB1/DepthLin lesen,
  Weltpos + `R = reflect(-V, N)` rekonstruieren, `intersection_query` gegen `m_giTlas`
  (Code-Vorlage: `giShadowRay` `:1342`, aber committed-hit statt any-hit).
- Output v1: `instanceColors[id].rgb` als Radiance, Confidence = hit?1:0, Miss → a=0
  (Composite fällt auf SkyEnv zurück — Sky-Reflektion gratis korrekt).
- Roughness-Cutoff im Kernel (früher Out bei `rough > maxRoughness`, a=0).
- Composite: `kSSRCompositeFS` um `giRefl`-Sampler + Kaskade erweitern (SYNC-Kommentare
  fortführen); Nicht-SSR-Fall: schlanker eigener `kGIReflCompositeFS` aus derselben Quelle.
- **Meilenstein:** he_shot-A/B — Off-Screen-Objekt spiegelt sich flach-farbig, wo SSR versagt.

### P2 — Hit-Shading über das GI-Feld („GI-basiert“ im Wortsinn)
- Im Kernel nach committed hit: `hitPos = origin + R·dist`;
  `radiance = albedo · (sunVisible·sunColor·NdL + giSampleFieldIrradiance(hitPos, hitN))·giIntensity`.
  Sonnen-Sichtbarkeit = ein zweiter any-hit-Ray (identisch `giShadowRay`-Logik, gleiche
  Winkelradius-Behandlung `lightRadius`).
- `giSampleFieldIrradiance` + `octEncode` aus `kGIProbeMSL` in `kGIReflMSL` duplizieren
  (Library-Grenzen; Konstanten in den Drift-Guard-Test aufnehmen, s. §7).
- Atlanten binden: `m_giIrradianceAtlas`/`m_giVisibilityAtlas` (+ GIUniforms-Twin als
  Kernel-Uniform, CPU-Struct `BuildGIUniforms` `:3623` wiederverwenden).
- Distanz-Fade (Confidence ↓ mit Hit-Distanz) gegen harte Grid-Rand-Artefakte;
  Hits außerhalb des Probe-Grids → nur Sonne+Ambient-Floor (Never-black-Floor wie im
  Probe-Kernel).
- **Meilenstein:** Reflektierte Objekte zeigen plausible Beleuchtung statt Flat-Color;
  Nacht/Tag-Konsistenz mit DDGI-Diffuse (HE_SKY_TIME-A/B).

### P3 — Glossy + Temporal + Quality-Tiers
- Roughness-abhängige Behandlung: scharfer Mirror unterhalb `roughCutoffSharp`, darüber
  Blur-Kette (SSR-`kSSRBlurFS` wiederverwenden — separates Ping/Rough-Target, Lerp nach
  Roughness im Composite, exakt das SSR-Quality-High-Muster `:9639`).
- Temporale Akkumulation kamerareprojektiv (Vorlage `giShadowTemporal` `:1237`:
  prevViewProj-Reproject, Depth-Reject, EMA) — halbiert das 1-Ray-Rauschen.
- Quality 0/1/2: Half/Half/Full-Res-Trace, Blur aus/an/an+Wide, Temporal an ab 1.
- **Meilenstein:** rauhe Metalle ohne Treppen-/Rauschartefakte in Bewegung (he_shot ist
  statisch — hier ein kurzer manueller HW-Check nötig).

### P4 — Echte Hit-Normalen + Albedo-Qualität (optional, aber notiert)
- Vertex/Index-Buffer der Unique-Meshes als Buffer-Array an den Kernel binden
  (`get_committed_primitive_id()` + baryzentrische Koords → interpolierte Normale);
  löst das `-rayDir`-Approx auch für den Probe-Kernel (dort als Follow-up markiert —
  gemeinsame Infrastruktur bauen, beide Kernel profitieren).
- Optional: `m_giInstanceColorBuffer` → `GIInstanceShading` erweitern (albedo + emissive +
  metallic), damit emissive Objekte in Reflektionen leuchten.
  → **UMGESETZT (albedo + emissive):** 2×float4 pro Instanz (HW-Buffer + SW-`GiInst`).
  Die Instanzfarben kommen jetzt aus `giInstanceShading`: Graph-Materialien über den
  CPU-Konstanten-Fold `HE::matGraphApproxSurface` (BaseColor-/Emissive-Pin; Const-Ketten
  gefaltet, Param-getriebene Pins als Slot-Index → liest den LIVE-Wert inkl. per-Entity-
  paramOverride), gebacken in `MaterialAsset::approxBaseColor/approxEmissive(+Slots)`,
  MTRL-Tail-serialisiert (Packer kopiert byte-verbatim). Vorher spiegelte JEDES
  Graph-Material grau (Asset-`baseColor` = weiß, Emissive existierte gar nicht).
  Metallic bleibt offen. Emissive-Bounce ins Probe-Feld bewusst NICHT (würde die
  globale Lichtbalance ändern, nicht nur Reflektionen).
  → **Metallic/Roughness NACHGEZOGEN + Multi-Bounce:** die Instanz-Paare packen jetzt
  (albedo.rgb, metallic) + (emissive.rgb, roughness) — Graph-Materialien über den
  Skalar-Fold von matGraphApproxSurface (Konstanten, kein Live-Slot), MTRL-Tail-append.
  Beide Kernel (HW+SW) laufen eine **Bounce-Schleife** (`GIReflectionSettings.bounces`
  1–4, `extra.w`; Editor-Slider „GI Refl Bounces", Config-Key `GIReflBounces`,
  `HE_DUMP_GIREFLBOUNCES`): mirror-artige Treffer (metallic × low-rough) reflektieren
  weiter, `throughput` trägt die Metall-Tönung, der Mirror-Anteil des lokalen Shadings
  wird einbehalten (das nächste Segment liefert ihn); Sekundär-Miss sampelt die
  Sky-Cubemap (Textur 8), Primär-Miss bleibt Confidence 0 (Composite-Fallback exakt).
  Witness: GIREFLTEST hat jetzt einen Spiegel-Slab — sein Bild im Spiegelboden ist der
  zweite Bounce (1 Bounce = flache Basisfarbe, ≥2 = verschachtelte Spiegelung).
  Witness: `HE_DUMP_GIREFLTEST=1` (Spiegelboden + Graph-Grün-Würfel + Emissive-Würfel).
  **Temporal-Lag-Fix (Quality 2):** History-EMA 0.85 → adaptiv — CPU-seitig auf 0.55
  gedämpft, sobald sich die View-Matrix ändert (Reflected-Content wird mangels Motion
  Vectors nicht reprojiziert → starres EMA = „Spiegelung zieht nach"), und im Kernel
  kollabiert das Gewicht bei Luminanz-Brüchen (bewegtes Objekt bei stehender Kamera),
  während Glossy-Jitter-Rauschen weiter voll akkumuliert.

### P5 — SW-Fallback + Forward-Anbindung
- `kernel giReflRaySw` in `kGISWMSL` (BVH-Scan-Vorlage `giShadowRaySw`/`giProbeUpdateSw`);
  Gate über `m_giHwRt`, `HE_GI_FORCE_SW`-A/B muss pixel-plausibel gleich sein.
- Forward-Tail (transparente Objekte, skinned) bleibt v1 bei SkyEnv — dokumentieren.
- `supportsScreenSpaceReflections`-Analogon: GIRefl braucht **kein** Tile-Deferred
  (nur stored G-Buffer) → Capability = `m_giSupported && deferredAktiv`.

### P6 — Verifikation + Doku
- `scripts/he_shot.py`-Serie: GIRefl on/off, SSR on/off-Matrix (4 Bilder), HW vs SW,
  Tag/Nacht, Roughness-Sweep-Testszene. Ich kann das selbst visuell prüfen (Metal headless).
- Drift-Guard-Test erweitern (§7), `docs/`-Update, MASTERPLAN-Forts., Memory.

## 5. Aufwandsschätzung (grob)

P0+P1 zusammen sind der Kern (~1 Session), P2 klein (~0.5, die Bausteine existieren alle),
P3 ~1 (Temporal ist der heikle Teil), P4/P5 je ~0.5-1. Nichts davon blockiert einander
hart außer P0→P1→P2.

## 6. Interaktion mit SSR — Entscheidungslogik im Composite

| Situation | Quelle |
|---|---|
| SSR-Hit mit Confidence | SSR (schärfste Daten) |
| SSR-Miss/Low-Conf, GIRefl-Hit | GIRefl (Feld-beleuchtet) |
| beide Miss | SkyEnv-Cubemap (wie heute) |
| `rough > maxRoughness` | SkyEnv/`ambSpec` (heutiger Pfad, `ssr.w`-Schalter greift nicht… siehe unten) |

Achtung Detail: `heLight.ssr.w` nullt `ambSpec` **pauschal**; der Composite muss also auch
im Rough-Cutoff-Fall SkyEnv selbst wieder auftragen (macht `kSSRCompositeFS` heute schon
genau so — Verhalten beibehalten).

## 7. Backend-Drift & Portabilität

- Metal-only zunächst. Neue geteilte Konstanten (Feld-Sampling im Refl-Kernel) in den
  Konstanten-Vergleichstest `tests/test_culling.cpp:1114` aufnehmen, sobald eine zweite
  Kopie entsteht; bis dahin: Kommentar-Marker `// SYNC:` wie im SSR-Composite.
- GL-4.3-Port später über `gi_refl.comp` analog `gi_shadow.comp` (CPU-BVH-Pfad existiert);
  D3D/Vulkan wie üblich blind nachziehen. Nichts im Plan verbaut das.

## 8. Risiken & bekannte Lücken (bewusst akzeptiert)

1. **Terrain + skinned Meshes fehlen in TLAS/BVH** → spiegeln sich nicht. Gleiches Loch wie
   GI-Diffuse/GI-Shadows heute; Fix wäre ein eigenes Projekt (Terrain-Chunks als BLAS).
2. **Probe-Grid statisch** (einmal gebaut, `m_giProbeGridBuilt`) → Hit-Shading außerhalb
   des Grids fällt auf Sonne+Floor zurück. Akzeptiert, Distanz-Fade mildert.
3. **Keine Motion Vectors** → Temporal ghostet bei bewegten Objekten; kameragebundene
   Reprojection + Depth-Reject wie beim GI-Shadow-Temporal ist der erreichbare Stand.
4. **Sampler-Cap 16**: alle neuen Texturen nur im Standalone-Pass, niemals ins
   Material-Preamble. (Composite-FS hat eigenes Budget, SSR belegt dort bereits Slots —
   beim Erweitern zählen.)
5. `m_giRaysPerProbe` ist tot (stored-but-unused) — beim Anfassen von `GISettings` nicht
   versehentlich „reparieren", das ändert Probe-Verhalten.
6. TLAS-Rebuild pro Frame ist heute schon der GI-Kostenblock; GIRefl fügt keinen zweiten
   Build hinzu (Reuse), aber der Trace selbst ist Full-/Half-Screen statt 64 Rays/Probe —
   Profiler-Scope von Anfang an, Budget im Auge behalten (Detailed-Capture F9).

## 9. Nachträge

- **2026-08-04:** Sonnen-Occlusion-Ray im Bounce-Loop wird übersprungen, wenn der
  Sonnenterm schwarz ist (Nacht) — das Sichtbarkeitsergebnis würde mit 0
  multipliziert (HW- + SW-Kernel identisch). SSR-seitig kam der Glossy-Cone-Jitter
  des Q2-Kernels auch im SSR-Trace an (High tier) — beide Reflexionsquellen
  bauen Glossy jetzt gleich auf (Cone-Jitter + Temporal + Wide-Blur-Lerp);
  Details in docs/ssr-plan.md („Trace v3").

## 10. GL-Port (Windows/Linux) — umgesetzt

> Stand: 2026-08-04. §7 hatte den Port als „später über `gi_refl.comp` analog
> `gi_shadow.comp`" geparkt; genau das ist er geworden, nur mit dem GLSL im
> Backend-String statt als Datei (der GL-Pfad hält alle Kernel als
> `R"GLSL(...)"` in `OpenGLRenderer.cpp`, die `shaders/*.comp` gehören Vulkan).
> `Capabilities::supportsGIReflections = m_giSupported` (GL 4.3 ⇒ Windows/Linux;
> macOS-GL ist 4.1 und bleibt bei Metal).

**Pass-Kette** (alles halbe Auflösung, auf den GI-Pre-Pass-Targets):

```
RenderGIPrepass   ← NEU herausgelöst aus RenderGIShadow: Weltpos + Normale
                    + (roughness, metallic) als 3. MRT-Ziel
RenderGIShadow    ← nur wenn GI-Diffuse an
RenderGIReflections: kGiReflCS → [kGiReflTemporalFS] → [kGiReflBlurFS ×2]
```

Der Pre-Pass läuft, sobald **einer** der beiden Verbraucher ihn will — GI-Reflektionen
sind ein eigener Toggle und funktionieren mit ausgeschaltetem Diffus-GI (Treffer werden
dann nur von Sonne + lokalen Lichtern + Ambient-Floor beleuchtet, ohne Probe-Feld).
`UpdateGIAccel` baut die BVH entsprechend bei `m_giEnabled || m_giReflEnabled`.

**Composite:** kein eigener Fullscreen-Pass wie auf Metal. GL komponiert *im
Shading-Pass* — die Kaskade `envSpec = mix(sky, refl.rgb, refl.a·intensity·fade)`
steht einmal in `kUnlitFS` (Built-in-PBR) und einmal in `heLitP`
(`MaterialShaderLibrary.cpp`, `heGIReflFwd` + `heLight.giRefl`, war für Metals
Forward-Pfad schon da). Beide sampeln dieselbe Textur auf **Unit 18**, einmal
gebunden. Folge: der Deferred-Pfad ist gratis mitversorgt (sein Resolve ruft
dasselbe `heLitP`), und es gibt keinen Tile-/Stored-G-Buffer-Zwang.

**Was der GL-Kernel BESSER kann als der Metal-Erstwurf:**
- **Echte Trefferflächennormale ohne Zusatzinfrastruktur.** `giSceneClosestHitTri`
  liefert den Dreiecksindex mit; `giHitNormal` baut daraus die geometrische
  Normale (Objektraum → Welt via `transpose(mat3(invTransform))`). Kein
  `-rayDir`-Ersatz, kein Argument-Buffer aus GPU-Adressen (Metal P4).
- **Exakte temporale Reprojektion.** Der Pre-Pass speichert Weltpositionen, also
  reicht `prevViewProj · worldPos` — keine Tiefenrekonstruktion.

**Was er NICHT kann (bewusst, nicht vergessen):**
1. **Kein Bounce-Loop.** `GIReflectionSettings::bounces` wird auf GL ignoriert
   (`SetGIReflectionSettings` dokumentiert das) — ein Spiegel im Spiegel zeigt
   seine flache Basisfarbe. Der Metal-Kernel hat die Schleife, GL nicht.
2. **Kein SSR darunter/darüber.** GL hat keinen Screen-Space-Trace; die Kaskade
   besteht nur aus Sky → GIRefl. Der `heLight.ssr.*`-Zweig faltet sich tot.
3. **Kein Sky-Cubemap-Sampling im Kernel** (das brauchte erst der Sekundär-Miss
   des Bounce-Loops). Primär-Miss = Confidence 0 → das Composite behält den
   Sky-Term, was exakt richtig ist.
4. Gleiche Lücken wie GI-Diffus: **Terrain und skinned Meshes sind nicht in der
   BVH**, spiegeln sich also nicht; skinned Empfänger stehen auch nicht im
   Pre-Pass (`cmds.drawCalls()` ist statisch) und fallen auf den Sky-Term zurück.

**Quality-Tiers** (gemessen am Witness, Laplace-Energie im Reflexionsband —
Reflektionen AUS = 1.65 als Referenz für den vorhandenen Dither):
`0` roh (1.29) · `1` + confidence-gewichteter separabler Blur (0.50, am
glattesten) · `2` + Cone-Jitter (erst ab roughness > 0.08 — darunter läuft eine
1°-Streuung an Silhouetten vorbei und dithert sichtbar) + temporale EMA mit
Nachbarschafts-Klammer (1.11). Die Klammer ist der Ersatz für Metals
Luminanz-Bruch-Test: ein hartes Reject würde die Akkumulation genau an den
Hit/Miss-Rändern der Jitter-Strahlen wegwerfen.

**Drift-Guard.** `sampleDDGIIrradiance` existiert damit in **vier** Kopien
(`kLightingPreamble`, `scene.frag`, GLs `kUnlitFS`, GLs `kGiReflCS`). Was §7 für
diesen Fall verlangt hat, ist umgesetzt: der Subcase „DDGI probe sampling on the
shading side" in `tests/test_culling.cpp` vergleicht jetzt alle vier (die beiden
GL-Kopien werden per String-Literal-Namen aus `OpenGLRenderer.cpp` geschnitten).
Negativkontrolle gelaufen: 0.05 → 0.06 im Chebyshev-Floor des Refl-Kernels lässt
den Test rot werden.

**Instanzfarben.** Neu geteilt: `HE::giInstanceSurface`
(`HorizonRendering/GiInstanceSurface.h`) löst albedo/emissive/metallic/roughness
eines `RenderObject` auf — inklusive `MaterialAsset::approx*`-Fold für
Graph-Materialien. Vorher nahm GL nur `obj.baseColor`, d.h. **jedes**
Material-Objekt war in Bounce UND Reflexion weiß. Metals `giInstanceShading` ist
dieselbe Auflösung und sollte auf die geteilte Funktion umgestellt werden (nicht
getan: hier nicht kompilierbar). GLs `GiInst` trägt jetzt das Metal-Layout
(baseColor.a = metallic, emissive.a = roughness) — der Bounce-Loop hätte alles,
was er bräuchte.

**Verifikation** (headless, `HE_DUMP_RHI=OpenGL` + `HE_DUMP_GIREFLTEST=1`; die
`HE_DUMP_*`-Familie ist backend-agnostisch, `scripts/he_shot.py` selbst ist
macOS-only):
A/B `HE_DUMP_GIREFL=0/1` — 11.8 % der Pixel ändern sich, der Rest ist identisch.
Der Spiegelboden zeigt an der Stelle des grünen Graph-Würfels (35,193,65) statt
Himmelblau (108,159,198), an der des Emissive-Würfels (251,178,178) — Albedo-Fold
und Emissive kommen also beide an. Boden abseits der Objekte und Hintergrund:
byte-identisch (Miss → Sky-Fallback).
**Deferred separat verifiziert** (`HE_DUMP_RENDERPATH=1`, dass der Pfad wirklich
greift zeigt `HE_DUMP_GBUFFER=2` → 50 % Bildänderung): dieselbe A/B liefert
dieselben Zahlen (11.8 %, 35,193,65 / 251,178,178). Damit ist die
heLitP-Hälfte der Kaskade belegt, nicht nur die `kUnlitFS`-Hälfte — im Forward
kommt man an sie nicht heran, weil die Graph-Würfel dort ohnehin nicht rendern
(s.u.). Beiläufig: Forward und Deferred sind in dieser Szene fast pixelgleich —
sichtbar sind nur Sky und zwei Spiegel, deren Erscheinung fast vollständig aus
`envSpec` kommt, und das rechnen beide Shader gleich.
**„Aus = unverändert"** gegen den Stand VOR dem Port: 45 von 230 400 Samples,
alle im animierten Wolken-Dither, max. Delta 22. Der Rauschboden zweier
identischer Läufe desselben Binaries liegt bei 33 Samples / Delta 18 — die
Änderung liegt also im Messrauschen, und jeder Szenen-Messpunkt ist byte-gleich.
Zusätzlich: die GLSL-Strings werden vom C++-Build **nie** kompiliert — vor jedem
Commit `glslangValidator` über `"#version 430 core\n" + kGiTraversalGLSL +
kGiReflCS` laufen lassen, sonst schaltet ein Tippfehler GI still für die ganze
Session ab (`m_giSupported = false`).

**Nicht von diesem Port verursacht, beim Verifizieren aufgefallen:** im
`HE_DUMP_GIREFLTEST`-Witness rendern die beiden Graph-Material-Würfel auf GL
**direkt gar nicht** (in der Reflexion schon — die läuft über die BVH, nicht über
das Material-Programm), und die erste Szenenframe meldet `glGetError=0x502`.
Beides reproduziert unverändert auf dem Stand vor diesem Port.

---

## Landschaften in der Reflexion (Nachtrag)

**Symptom:** Ein gespiegeltes Landscape war weiß — die Geometrie stimmte, die
Farbe fehlte komplett. SSR war davon **nicht** betroffen (Begründung unten).

**Ursache:** Ein Ray-Hit wird pro INSTANZ geshadet, es gibt keine UV und keine
Material-Auswertung — die Farbe kommt aus dem CPU-Fold `matGraphApproxSurface`.
Der konnte einen `Landscape Layer Blend` gar nicht falten (`default: return
false`), und ebenso wenig die prozeduralen Generatoren. Ein reales
Landschafts-Material trifft beides: BaseColor hängt am Layer-Blend, und jede
Layer-Farbe ist typischerweise `Farbe × FBM`-Mottling. „Linked but unfoldable"
heißt im Fold: Default behalten → **weiß**.

**Fix in drei Teilen:**

1. **Generatoren falten zu ihrem analytischen Mittelwert** statt zu scheitern:
   `heValueNoise` → 0.5, `heFbm`/`heFbm3` → 0.46875 (vier Oktaven,
   0.5+0.25+0.125+0.0625 = 0.9375 Amplitude × 0.5), `Checker` → 0.5. Damit
   überlebt die `Farbe × Noise`-Kette ihre Farbe. Mitgenommen: `Power`, `Sine`,
   `Step`, `Smoothstep`, `Absolute`, `Fract`, `Combine3`, `CombineRGBA`.
2. **Layer-Blend wird gesplittet, nicht gemittelt.** `MatApproxSurface` trägt
   jetzt `layerColor[4]` + `layerCount` (→ `MaterialAsset::approxLayerColor/
   approxLayerCount`, MTRL-Tail angehängt, Packer re-foldet mit). `baseColor`
   bleibt als paint-agnostischer Fallback der Layer-Durchschnitt.
3. **Der Hit sampelt die Paint PRO TEXEL.** Erster Anlauf war ein Mittel der
   Weightmap über das ganze Terrain (`TerrainComponent::avgLayerWeights`, einmal
   pro Paint berechnet) — das machte aus „weiß" zwar „die Farbe des Landscapes",
   aber eben **eine** Farbe: ein roter Streifen auf grünem Hang spiegelte als
   Mischton. Der Mittelwert bleibt als Fallback (DDGI-Probe-Bounce, Landscapes
   jenseits des Kernel-Caps), die Reflexion sampelt jetzt echt.

   Möglich ist das, weil ein Landscape die **einzige** Fläche ist, deren
   fehlende UV sich rekonstruieren lässt: Heightfield über einem achsparallelen
   lokalen XZ-Rechteck, und `TerrainMeshGenerator` schreibt GLOBALE (nicht
   per-Chunk) UVs — die Welt-Trefferposition liefert die UV also exakt. Keine
   Vertex-UVs in der Beschleunigungsstruktur, kein Vertex-Format-Umbau, und in
   allen drei Kerneln (Metal HW, Metal SW-BVH, GL 4.3) identisch.

   `HE::GiLandscape` (`RenderWorld::landscapes`, gefüllt vom Extractor) trägt
   `worldToLocal` + Größe + die vier Layer-Farben; `RenderObject::
   landscapeIndex` zeigt vom Chunk darauf. Der Kernel rechnet Treffer → lokal →
   UV, sampelt die Weightmap-Textur (dieselbe, die der Rasterizer bindet, mit
   demselben clamp+linear) und blendet die Layer-Farben. Cap
   `HE::kGiMaxLandscapes = 4` (eine Textur-Bindung pro Landscape); darüber
   greift der flache Mittelwert.

Nebenbei: Metals `giInstanceShading` war eine handgepflegte Zweitkopie derselben
Auflösung (der Header warnte davor) und ist jetzt ein dünner Adapter über
`HE::giInstanceSurface` — eine Quelle für alle Backends.

**Warum SSR nicht betroffen ist:** Der SSR-Trace liest die Trefferfarbe aus
`heSceneColor`, also aus dem fertig geshadeten Bild — Material, Texturen und
Layer-Blend sind dort schon drin. SSR kann die Materialfarbe strukturell nicht
verlieren; sein Ausfallmodus ist ein anderer (kein Screenspace-Treffer → Sky-
Cubemap-Fallback).

**Witness:** `HE_DUMP_GIREFLLANDSCAPE=1` — Landscape mit Layer 0 = grün × FBM
überall und Layer 1 = rot als **Band** quer darüber, dazu zwei Spiegel: ein
kamerazugewandter (nur die Ray-Reflexion kann ihn beantworten, SSRs Facing-Gate
verwirft solche Strahlen per Konstruktion) und ein um 55° gegierter (dessen
Strahlen laufen quer durchs Bild → SSR trifft). Der Spiegel muss das Band an
der richtigen STELLE zeigen; ein einfarbiger Spiegel — grün oder gemischt — ist
die Regression. `=2` flutet stattdessen den roten Layer über alles.
Verifiziert auf Metal (macOS, `scripts/he_shot.py`); der GL-Kernel ist wie beim
Port selbst blind geschrieben und offline mit `glslangValidator` über
`"#version 430 core" + kGiTraversalGLSL + kGiReflCS` geprüft (ebenso Probe und
Shadow, die sich denselben Traversal-Block teilen).

**Stolperstein beim Bauen, fürs nächste Mal:** `RenderWorld::clear()` räumte die
neue `landscapes`-Liste nicht mit auf. Die wuchs also pro Frame, die Indizes der
Chunks liefen über `kGiMaxLandscapes` hinaus, der Kernel verwarf sie stillschweigend
— und der Spiegel sah exakt so aus wie vor dem Fix. Jedes neue `RenderWorld`-Feld
gehört in `clear()`.

**Offen (gleiche Klasse, nicht von diesem Fix abgedeckt):** Ein `Texture
Sample` auf der BaseColor faltet weiterhin nicht — ein texturiertes Material
spiegelt seinen Skalar-`baseColor` (meist weiß). Sauberer Weg wäre eine
Durchschnittsfarbe der Textur (gebacken wie `approx*`), analog zu dem, was hier
für die Layer passiert ist.

---

## Warum in Spiegeln „immer der Himmel drin" war (Nachtrag 2)

Die Kaskade ist **geschichtet**: Basis ist die prozedurale Sky-Cubemap
`heSkyEnv`, GI-Reflexion und SSR werden per Confidence darüber gemischt
(`envSpec = mix(envSpec, rr.rgb, rr.a * intensity * fade)`). Alles, was nicht
volle Confidence hat, lässt also Himmel durch. Vier Gründe, warum das öfter
passierte als es sollte:

1. **Der Bug:** die Confidence eines Ray-Treffers war
   `roughFade * (1 - 0.25 * dist/maxDist)` — sie erreichte **nie 1**. Ein
   perfekter Treffer bei 200 m behielt 25 % Himmel bei, als weicher Verlauf
   über die Fläche. Die Formel stammt aus dem SSR-Trace, wo Confidence mit der
   Marschdistanz zu Recht sinkt (die Evidenz wird dünner); ein getracter
   Treffer ist bei jeder Distanz **exakt**. Ersetzt durch die Form, die der
   GL-Kernel schon hatte: volle Confidence bis 75 % der Reichweite, Abblenden
   nur im letzten Viertel, wo „Reichweite zu Ende" der echte Fehlerfall ist.
2. Die Cubemap ist **nicht die Szene**, sondern reiner Atmosphären-Himmel mit
   einer *pauschalen* Bodenhälfte (`ground = mix(sky*0.32, vec3(0.24,0.23,0.21),
   day)` unterhalb des Horizonts, `SkyEnvBake.h`). Ein nach unten zeigender
   Strahl, der nichts trifft, bekommt also ein generisches Graubraun statt des
   Terrains.
3. **Primär-Miss = Confidence 0 = reine Cubemap** — per Design, damit der
   Composite-Fallback exakt ist. Bei `maxDistance` 200 m (Default) ist ein
   flacher Strahl über ein großes Landscape schnell ein Miss.
4. `ambSpec = envSpec * fresnel * (1 - 0.6*rough)` plus der Roughness-Fade
   Richtung Cubemap oberhalb `0.7 * maxRoughness`.

1 ist behoben; 2–4 sind Designgrenzen der Kaskade (echte Reflection-Probes wären
die Antwort auf 2, eine größere `maxDistance` auf 3 — beides mit Kosten).

## Warum „High" verrauschter aussah als „Medium"

High (Quality 2) heißt: Glossy-Cone-**Jitter**, EIN Strahl pro Pixel, auf halber
Auflösung. Der einzige Integrator dieser stochastischen Schätzung ist die
temporale EMA — und die muss unter Kamerabewegung gedämpft werden (0.85 → 0.55),
weil der reflektierte Inhalt mangels Motion Vectors nicht reprojiziert wird; im
Kernel kollabiert sie zusätzlich bei Luminanz-Brüchen bis ~0.14. Ergebnis: beim
Navigieren zahlte der Tier die volle Jitter-Varianz bei halber Integration,
während Medium (deterministischer Strahl + Blur) sauber blieb. Der höhere Tier
sah also schlechter aus — eine Inversion, kein Geschmacksfrage.

Fix: der Cone folgt jetzt **demselben Signal** wie die EMA. Bewegt sich die
Kamera, schließt er sich (→ deterministischer Strahl, exakt Medium-Verhalten,
null Varianz); steht sie still, öffnet er sich über ~8 Frames wieder und die EMA
integriert ihn zu einer echten Glossy-Lobe. Kostet keine zusätzlichen Strahlen.
Metal und GL identisch (`GIReflParams::land.y` / `uJitterScale`).

### Nachtrag: der Cone-Ramp allein reichte nicht

Rückmeldung nach dem obigen Fix: beim Bewegen sauber (wie Medium), beim
**Stehenbleiben** aber sogar stärker verrauscht als vorher. Zwei echte Ursachen
dahinter, die der Ramp nur sichtbarer gemacht hat:

1. **Der breite Glossy-Blur wurde im Forward-Pfad nie gesampelt.** Quality 2
   rechnet zwei Blur-Stufen: schmal (5-Tap) nach `m_giReflTex`, breit (3-Texel)
   nach `m_giReflRoughTex`. Der DEFERRED-Composite lerpt beide per Pixel nach
   G-Buffer-Roughness. Der Forward-Pfad hat keinen Composite — sein Scene-Shader
   sampelt **eine** Textur, und das war immer die schmale. Der breite Blur wurde
   also jeden Frame berechnet und weggeworfen; eine glossy Fläche bekam einen
   einzigen gejitterten Strahl pro Pixel durch einen 5-Tap-Blur. Der Editor läuft
   per Default forward — genau der Pfad, in dem es auffiel. Fix: ein kleiner
   Fullscreen-Pass (`kSSRRoughMixFS`) backt denselben Lerp in die Textur, mit der
   Roughness aus dem Reflexions-Prepass (`heGB1.b`, gleiche Halbauflösung). Kein
   zusätzlicher Sampler-Slot — das Metal-Budget im Scene-Shader ist voll.
2. **Die adaptive History deutete Jitter-Rauschen als Inhaltswechsel.**
   `hEff = h * (1 - 0.75*smoothstep(0.2, 0.8, rel))` sollte eine EMA einbrechen
   lassen, wenn sich der reflektierte INHALT ändert. Bei offenem Cone ist ein
   großer Frame-zu-Frame-Luminanzsprung aber genau das Sampling-Rauschen — die
   Bedingung bestraft also die Integration, die dieses Rauschen braucht, und ist
   selbsterhaltend: verrauscht → Kollaps → verrauschter. Effektiv ~1.2 statt ~7
   akkumulierte Samples. Der Kollaps blendet jetzt mit dem Cone aus (Spiegel:
   volle Bewegt-Objekt-Ablehnung, glossy: vertraut seiner History), und das
   Still-Gewicht steigt 0.85 → 0.94 (≈17 statt ≈7 effektive Samples).

   **Der GL-Kernel löst dasselbe Problem sauberer** — mit einem 3×3-
   Neighbourhood-Clamp (`kGiReflTemporalFS`), der beide Garantien behält statt
   eine gegen die andere zu tauschen. Er kann das, weil seine Temporal ein
   eigener Fullscreen-Pass über den rohen Trace ist; in Metal ist sie IN den
   Trace-Kernel gefused, wo ein Thread nur sein eigenes Sample hat und ein
   Threadgroup-Austausch eine Barriere bräuchte, die die Early-Outs dieses
   Kernels unsicher machen. Den Pass aufzuspalten ist der echte Fix und lohnt
   sich, falls glossy Reflexionen BEWEGTER Objekte je sichtbar nachziehen.

**Ehrlichkeitshinweis zur Verifikation:** headless lässt sich das NICHT zeigen —
der Dump rendert aus einer stehenden Kamera, und genau dort konvergiert die EMA
ohnehin (gemessen: Quality 1 und 2 haben denselben Hochfrequenz-Gradienten in der
Spiegelfläche). Die Diagnose kommt aus dem Code, der Fix entfernt den
Mechanismus; bestätigen lässt er sich nur beim Navigieren im Editor.


---

## Quality-Tiers heißen jetzt: Rays + Blur

Nach zwei Runden Nachbessern am Rauschen war klar, dass die Tier-Semantik selbst
das Problem war. Sie schaltete FEATURES um — Low roher Trace, Medium + Blur,
High + gejitterter Cone mit temporaler Akkumulation — und der oberste Tier
tauschte damit einen deterministischen Strahl gegen eine stochastische Schätzung
ein, deren einziger Integrator die temporale EMA war. Die muss unter
Kamerabewegung gedämpft werden (kein Motion-Vector-Reprojection des
reflektierten Inhalts), also war „High" genau dann schlechter als „Medium", wenn
man sich bewegt — und beim Stehenbleiben hing alles an einer EMA, die ihr
eigenes Rauschen als Inhaltswechsel missdeutete.

Jetzt bedeutet der Tier **zwei Zahlen und sonst nichts**:

| Tier | Rays/Pixel | Blur |
|---|---|---|
| Low | 1 (deterministisch) | keiner |
| Medium | 2 | schmal (1 Texel, 5-Tap) |
| High | 4 | schmal + breit (3 Texel) mit Roughness-Lerp |

Die Strahlen sind über Sample-Index UND Frame stratifiziert, also verteilt sich
schon ein einzelner Frame über die Lobe statt zu klumpen. Ein **Near-Mirror
traced immer genau einen** Strahl (sein Cone ist schmaler als ein Pixel, alle
Samples wären identisch) — Spiegel kosten auf jedem Tier dasselbe, die Kosten
wachsen nur dort, wo die Reflexion wirklich glossy ist. Die temporale EMA bleibt,
ist aber jetzt Zugabe auf getracte Strahlen statt der Integrator, an dem das Bild
hängt; entsprechend darf sie unter Bewegung konservativ bleiben, ohne dass der
Tier zusammenbricht.

Damit fällt der Cone-Ramp aus dem vorigen Nachtrag ersatzlos weg — er war eine
Kompensation dafür, nur einen Strahl zu haben. Metal (HW + SW-BVH) und GL sind
identisch aufgebaut; der GL-Kernel mittelt zusätzlich Misses als Confidence 0
ein, sodass eine halb ins Leere zeigende Lobe proportional Himmel durchlässt —
was sie soll.
