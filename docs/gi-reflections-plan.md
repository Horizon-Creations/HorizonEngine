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
