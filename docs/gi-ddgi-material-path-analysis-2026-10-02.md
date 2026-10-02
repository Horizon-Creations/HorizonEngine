# DDGI auf D3D11, D3D12 und Vulkan: Analyse (Thema 120, Schritt 1)

Stand: 02.10.2026, Zweig `claude/global-illumination-ddgi-ambient-term-fehlt-auf-d3d11-d3d12-`
auf `0513f8d4`. Reine Analyse, kein Code geändert.

## 0. Kurzfassung

**Die Prämisse des Themas stimmt nicht.** Ray-traced DDGI läuft heute auf allen fünf Backends.
Der eingebaute PBR-Shader sampelt das Probe-Feld überall:

- Software-RT gibt es auf Metal, GL 4.3, Vulkan, D3D11 und D3D12.
- Hardware-RT gibt es auf Metal, Vulkan (`VK_KHR_ray_query`) und D3D12 (DXR 1.1 RayQuery).

Das Gap-Audit (`docs/gap-audit-2026-08-25.md:67`, `:423`) hält das schon fest.

**Die echte Lücke ist schmaler: der Graph-Material-Pfad.** `heLitP()` (die gemeinsame
Lighting-Preamble aller Node-Graph-Materialien) hat seit Lighting-ABI v2.5 einen DDGI-Zweig,
`heGIIrradianceAt()`. Er hängt am Gate `heLight.giProbe.y`. Nur GL und Metal füllen dieses Gate
und binden die Atlanten. Auf D3D11, D3D12 und Vulkan bleibt `giProbe` 0.

Die Folge: Bei aktivem GI bekommt ein Objekt mit Graph-Material nur flaches Ambient plus
Spekular-IBL, ein Nachbarobjekt mit eingebautem Material aber Probe-Irradiance mit
Color-Bleeding. Graph-Materialien wirken dort bei GI also flacher und dunkler als ihre Umgebung.

**Vermutliche Quelle der falschen Prämisse:** `src/HE_Core/include/Renderer/IRenderer.h:233-237`
behauptet weiterhin „Metal-only … False on every other backend". Das Gap-Audit hat schon
`IRenderer.h:145-150` als veraltet markiert, `:233` ist eine zweite solche Stelle. Ebenso veraltet
ist `VulkanRenderer.cpp:8112` („GetCapabilities keeps supportsGlobalIllumination = false"),
während `:890` längst `true` meldet.

**Empfehlung an den Chefchen:** das Thema umbenennen, etwa in „DDGI-Ambient fehlt im
Graph-Material-Pfad (heLitP) auf D3D11/D3D12/Vulkan". Die Folgeschritte sind dann die drei
Backend-Verdrahtungen aus §3, kein RT-Port.

## 1. Ist-Stand der DDGI-Architektur (Antwort auf die Schrittfrage)

### 1.1 Gemeinsamer Kern (backend-neutral)

| Baustein | Ort | Inhalt |
|---|---|---|
| CPU-BVH | `src/HE_Rendering/include/HorizonRendering/GiBvh.h` | Median-Split, std430-Layout (32-B-Node, 48-B-Tri), de-indizierte Leaf-Reihenfolge; Test `tests/test_gi_bvh.cpp` |
| Probe-Grid-Fit | `HorizonRendering/GIProbeGrid.h` | Grid über die Szenen-AABB, Spacing wächst mit der Szene, `kGIProbeOctSize` = 8 |
| Landscape/Instanz-Flächen | `GiLandscape.h`, `GiInstanceSurface.h` | Terrain und Instanz-Albedo in die Accel-Strukturen |
| Material-Preamble | `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:255-350` (`heGIIrradianceAt`), `:625` (Zweig) | dieselbe Probe-Abfrage wie die eingebauten Shader |

Pro Backend gibt es fünf Stufen:

1. Accel-Build (TLAS/BLAS bzw. GiBvh in Buffern), aus derselben Extraction wie der Szenen-Pass.
2. Halbaufgelöster Welt-G-Buffer-Prepass.
3. Sonnen- und Lokal-Schatten-Kernel mit Temporal und Neighborhood-Clamp.
4. Probe-Update-Kernel: Gather, ein Thread pro Oktaeder-Texel, adaptive Hysterese, Multi-Bounce über
   Feedback aus dem Feld.
5. Sampling im Szenen-Shader: trilinear über acht Probes, Chebyshev-Visibility.

### 1.2 Pro Backend: Erkennung, HW-RT, Fallback

| Backend | Capability | HW-RT | SW-Fallback | Eingebauter Shader sampelt DDGI |
|---|---|---|---|---|
| Metal | `MetalRenderer.mm:16977`, HW-Wahl `:7474` | ja: `intersection_query`, `device.supportsRaytracing` | `kGISWMSL` über GiBvh (`:7773`) | ja (`fragmentMain`, `BuildGIUniforms`) |
| OpenGL | `OpenGLRenderer.cpp:3082` (`GLAD_GL_VERSION_4_3`) | — (keine API) | GLSL-430-Compute über GiBvh-SSBOs | ja; macOS-GL 4.1 planmäßig ohne GI |
| Vulkan | `VulkanRenderer.cpp:890` (immer true, Compute ist Core) | ja: `VK_KHR_ray_query` + accel + BDA, Probe `:1072-1107`; `gi_*_hw.comp` | `gi_*.comp` über GiBvh | ja (`shaders/scene.frag:467`) |
| D3D11 | `D3D11Renderer.cpp:6785` (`giSupported`, FL11.0 ⇒ CS 5.0, `:2888`) | — (keine API) | HLSL-CS über GiBvh-StructuredBuffers (`HlslSources.h:873`) | ja (`D3D11Renderer.cpp:758`) |
| D3D12 | `D3D12Renderer.cpp:10557` | ja: DXR 1.1 RayQuery in Compute (SM 6.5), `HE_D3D12_DXR`, Tier-Check `:7514`; `gi_*_hw.hlsl` via dxc | dieselben HLSL-CS wie D3D11, BVH als Root-SRVs | ja (`D3D12Renderer.cpp:783`) |

`HE_GI_FORCE_SW=1` erzwingt den SW-Pfad auf Metal, Vulkan und D3D12. Die Fragen des Schritts
sind damit beantwortet:

- D3D12 prüft und nutzt Hardware-RT bereits (Device5, `RaytracingTier >= 1_1`).
- Vulkan prüft und nutzt Hardware-RT bereits (Features2/pNext, `rayQuery`).
- Der CPU-BVH-Fallback (`HE::GiBvh` + Compute-Traversal) existiert für D3D11 und RT-lose GPUs.

Dort gibt es nichts mehr zu bauen.

**Was offen bleibt, aber nicht Gegenstand dieses Themas ist:** Die GI-*Optik* auf D3D11, D3D12 und
Vulkan ist nie auf echter Windows- oder Linux-Hardware gelaufen. Die Code-Struktur ist durch die
CI bewiesen, die Pixel sind es nicht. Das ist dieselbe Altlast wie bei allen Blind-Ports.

## 2. Die echte Lücke: Graph-Materialien bekommen kein DDGI

### 2.1 Wer füllt was im `Lighting`-Block (`MaterialShaderLibrary.h:86-94`)

| Feld | Bedeutung | GL | Metal | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|---|---|
| `giGridOrigin/Counts`, `giProbe` | DDGI-Feld + Gate | `OpenGLRenderer.cpp:10726-10739` | `MetalRenderer.mm:13966-13975` | **nein** (`fillMatLight` `:5721`) | **nein** (`:9276`) | **nein** (`:6378`) |
| `fog[2]` | heSkyEnv gebunden | ja `:10715` | ja `:13948` | nein | nein | nein |
| `fog[3]` | heAO gebunden | ja `:10716` | ja `:13949` | nein | nein | nein |
| `giParams.z` | GI-Masken | ja | ja | ja | ja | ja |

Die Schattenmasken (`giParams.z`) sind also überall verdrahtet, das Probe-Feld nirgends außer auf
GL und Metal. `LightPacking.cpp` (`FillMaterialLightWindow`, `FillMaterialWind`) fasst keines dieser
Felder an. Jede Fill-Stelle macht es selbst, GL und Metal doppelt.

Bei `giProbe.y == 0` nimmt `heLitP` den Else-Zweig `(ambDiff*0.35 + ambSpec)*ao + ambient*diffuse`.
Ohne `fog.z` ist `ambDiff` dort nur `diffuseColor * heLight.ambient`
(`MaterialShaderLibrary.cpp:563-567`). Der eingebaute Shader auf denselben Backends nimmt dagegen
`sampleDDGIIrradiance(...) * base * kd * giIntensity` (D3D11 `:757-759`, D3D12 `:783`,
`scene.frag:466-469`).

### 2.2 Bindings: was das Gate allein nicht löst

Die Preamble referenziert heGIIrradiance (Binding 17) und heGIVisibility (18) statisch. Die
HLSL-Pins (`MaterialShaderLibrary.cpp:2465-2489`) legen fest: SRV t17/t18, Sampler **s1/s3**.

**D3D12, klein.** Die Root-Signatur deckt t17/t18 schon ab
(`D3D12MaterialRootSignature.h:32-33`). s1 und s3 sind statische Linear-Clamp-Sampler (`:158`).
In der Staging-Vorlage stehen in den Slots 12/13 heute Null-Views (`D3D12Renderer.cpp:8202-8215`,
Kommentar: „wiring the real targets through here is the D3D11-parity job"). Jeder Draw kopiert den
Block per `CopyDescriptorsSimple` (`:9851`). Es reicht also, die echten Atlas-SRVs in die
Vorlage zu schreiben.

Der richtige Ort dafür ist `ensureGiProbeAtlas` (`:7034`): Es läuft hinter `waitForAllFrames()`
(`:7038`), und die Vorlage ist ohnehin nur CPU-seitig. Laufende Frames haben ihre Kopie schon.

`retireGiProbeAtlas` (`:7016`) muss die Slots 12/13 wieder auf Null-Views setzen. Sonst zeigt die
Vorlage nach einem Refit ohne neuen Atlas auf eine ausgemusterte Ressource. Das Gate stünde zwar
auf 0, aber ein Descriptor auf eine freigegebene Ressource ist auf D3D12 trotzdem undefiniert.

**D3D11, mittel, mit echter Registerkollision.** Der eingebaute Szenen-Shader belegt im selben Pass
dieselben Register anders:

| Register | eingebauter Shader (gebunden im Szenen-Pass) | Material-Preamble erwartet |
|---|---|---|
| t16 | `uSSRFwd` Texture2D (`:458`, Bind `:6088`) | heAO (texelFetch, `fog.w`) |
| t17 | `uLocalShadowMap` **Texture2DArray** (`:462`, Bind `:6066`) | heGIIrradiance Texture2D |
| t18 | `uClusterLights` **StructuredBuffer** (`:469`, Bind `:5644`) | heGIVisibility Texture2D |
| s1 | `pointSampler` (AO, `:6067`) | heGIIrradiance → braucht Linear-Clamp |
| s3 | `shadowSampler` Point/Clamp (`:6061`) | heGIVisibility → braucht Linear-Clamp |

Heute ist das nur deshalb harmlos, weil das Gate 0 ist und der Materialshader die Register nie
liest. Für DDGI muss der Material-Draw (`:6300-6330`) zusätzlich:

- t17/t18 mit `giIrrSRV`/`giVisSRV` belegen (`:2937`),
- s1/s3 mit `giLinearClamp` belegen,
- nach dem Draw die Werte des eingebauten Passes zurücklegen, also t17 lokaler Atlas, t18
  Cluster-Buffer, s1 Point, s3 Shadow.

Den Rückweg gibt es schon für t2/t4..t7/s2 (`:6390-6405`). Dazu kommt
`D3D11MaterialBindings.h` (Thema 57), das genau dieses Bind-plus-Restore-Paar für t14/s0 kapselt
und vom WARP-Test in `test_material_graph.cpp` gefahren wird. Dort gehört ein zweites Paar
`BindDDGIAtlases`/`RestoreBuiltinT17T18` hin. Ohne Restore würde der nächste eingebaute Draw seinen
Lokal-Schattenatlas als 2D-Textur und die Cluster-Lichter als Textur lesen. Die D3D11-Runtime
entbindet bei Typkonflikten nicht, sie liefert falsche Daten.

**Vulkan, am größten.** Das Material-Descriptor-Set-Layout (`VulkanRenderer.cpp:2318-2353`) hat
15 Bindings: 0-13 und 31. Die Preamble referenziert aber auch 15, 16, 17, 18, 32 und 33 statisch.
Der Code sagt das selbst (`:2343-2345`: „Bindings 15-18, 32 and 33 of the preamble are still
unbound here — that is a pre-existing gap"). Eine Pipeline, deren Shader ein Binding statisch
nutzt, das im Layout fehlt, ist spec-widrig (Pipeline-Layout-VUIDs zu `vkCreateGraphicsPipelines`).
Dass es läuft, ist Treiber-Glück, kein Vertrag. Für DDGI reichen 17/18 technisch.

Empfohlen ist, in einem Zug **alle sechs** fehlenden Bindings aufzunehmen, damit das Layout
spec-konform wird:

- 15 Cube aus Weiß-Fallback oder Null-Cube-View,
- 16 Weiß, wie heute Binding 10/11,
- 17/18 echte Atlanten (liegen in `GENERAL`, Storage+Sampled, also direkt sampelbar),
- 32/33 Weiß.

Folgen:

- DSL 15 → 21 Bindings.
- Pool-Größe: CIS pro Draw +6.
- Per-Draw-Writes +6.
- Das Pipeline-Layout ändert sich, alle Material-Pipelines entstehen neu. Das ist kein ABI-Bruch
  für Assets, weil das SPIR-V unverändert bleibt.
- Die Atlanten werden bei einem Refit neu erzeugt (`m_giIrrAtlas`, `VulkanRenderer.h:1138`). Die
  Descriptor-Ringe pro In-Flight-Frame werden pro Draw geschrieben. Wie bei den GI-Masken muss der
  Write auf das aktuelle Image oder auf Weiß zeigen, wenn kein Atlas existiert.

### 2.3 Nebenbefunde (nicht Teil des Themas, nur damit niemand sie neu entdeckt)

1. **Vulkan: Forward-SSR für Graph-Materialien ist tot.** `lit.ssr[0]` wird gesetzt
   (`VulkanRenderer.cpp:6438`). Der `heLight.ssr.x`-Zweig liegt in der Preamble aber *innerhalb*
   von `if (heLight.fog.z > 0.5)` (`MaterialShaderLibrary.cpp:565-584`), und Vulkan setzt `fog[2]`
   nie. Auf Vulkan gibt es keinen Sky-Env-Cube, also kann es das Gate auch nicht ehrlich setzen. Der
   Fix ist ein Preamble-Umbau: den SSR-/GI-Refl-Mix aus dem `fog.z`-Block lösen, mit Sky-Ersatz
   `ambient`. Das gehört zum SSR-Thema (`docs/ssr-cross-backend-plan.md`), nicht hierher.
2. **Parität bei GI aus:** D3D11, D3D12 und Vulkan haben keinen Sky-Env-Cube (`heSkyEnv`). Ihr
   eingebauter Shader rechnet `ambDiff` analytisch aus dem Himmel, das Graph-Material bekommt flaches
   `ambient`. Auch das ist eine Material-gegen-Built-in-Abweichung, aber unabhängig von DDGI.
3. **heAO (`fog[3]`)** fehlt aus demselben Grund auf allen dreien. Bei aktivem GI ist das egal, weil
   der DDGI-Zweig AO umgeht. Bei GI aus ist es ein eigener Paritätsfehler. Wer t16 auf D3D11 ohnehin
   umbelegt (s. o.), kann AO mitnehmen.
4. **Veraltete Kommentare:** `IRenderer.h:233-237` und `VulkanRenderer.cpp:8112` (s. §0).

## 3. Vorschlag für die Folgeschritte

### Schritt 2: gemeinsamer Fill + D3D12 (≈ 0,5 Tag)

- Neuer Helfer `HE::FillMaterialGIProbe(Lighting&, origin, spacing, counts, perRow, intensity,
  bool atlasesBound)` in `LightPacking.cpp`, neben `FillMaterialLightWindow`. GL und Metal stellen
  darauf um (reine Refaktorierung, Bild bitgleich). Unit-Test auf den Helfer: Gate 0, wenn die
  Atlanten fehlen.
- D3D12: Helfer in `fillMatLight` aufrufen, Gate nur bei `giShadingActive && giIrrTex && giVisTex`.
  Slots 12/13 der Staging-Vorlage in `ensureGiProbeAtlas` schreiben, in `retireGiProbeAtlas`
  wieder auf Null setzen. `D3D12MaterialRootSignature.h:39-41` anpassen.
- Prüfung: macOS-Build, `test_material_graph` (Registertest), Windows-CI (WARP-PSO-Test besteht
  weiter, weil sich die Signatur nicht ändert).

### Schritt 3: D3D11 (≈ 1 Tag)

- Fill über den Helfer.
- In `D3D11MaterialBindings.h` ein Bind-und-Restore-Paar für t17/t18 + s1/s3 (s. §2.2),
  aufgerufen im Material-Draw und vor dem Rückweg. Optional heAO auf t16 mitnehmen (`fog[3]`).
- WARP-Test erweitern: Nach dem Material-Draw liegen t17/t18/s1/s3 wieder auf den Werten des
  eingebauten Passes (wie der t14/s0-Test).

### Schritt 4: Vulkan (≈ 1-1,5 Tage)

- Fill über den Helfer.
- DSL 15 → 21 (15/16/17/18/32/33), Pool, Per-Draw-Writes, Weiß- bzw. Null-Cube-Fallbacks.
- Prüfung: glslang/SPIR-V-Reflexion des Material-Fragments gegen die DSL-Bindingliste (das geht
  ohne Gerät, siehe MoltenVK-Rezept), clang-Syntaxcheck mit MoltenVK-Headern, Windows-CI.
- Nebenbefund 1 (SSR tot) im SSR-Thema melden, hier nicht fixen.

### Reihenfolge und Begründung

**D3D12 → D3D11 → Vulkan**, nach Aufwand. D3D12 braucht keine neue Bindestruktur, beweist den
gemeinsamen Helfer und liefert auf der Hardware mit dem häufigsten DXR-Pfad das sichtbarste
Ergebnis. D3D11 hängt am bestehenden Restore-Muster. Vulkan zuletzt, weil es den größten Umbau
(Layout) hat. Dieser Umbau schließt aber zugleich die spec-widrige Layout-Lücke.

Gegenargument: Will der Chefchen die Vulkan-Layout-Lücke (VUID) dringlich schließen, kann Schritt 4
vorgezogen werden. Er hängt nur am Helfer aus Schritt 2.

## 4. Was sich verifizieren lässt und was nicht

| Prüfung | möglich hier? |
|---|---|
| Fill-Helfer (Gate, Werte) per Unit-Test | ja (macOS) |
| HLSL-Register-/Sampler-Regel (`test_material_graph.cpp`) | ja (macOS) |
| D3D12-PSO gegen Root-Signatur (WARP) | Windows-CI |
| D3D11 Bind/Restore (WARP) | Windows-CI |
| Vulkan-Layout gegen SPIR-V-Reflexion | ja (glslang lokal) |
| C++-Compile D3D11/D3D12/Vulkan | Windows-CI (Vulkan zusätzlich clang-Syntax mit MoltenVK) |
| **Bildparität Graph-Material vs. Built-in bei GI an** | **nein**: braucht Windows-/Linux-Hardware. Zeuge wäre `HE_DUMP_GIBLEED` mit einer Graph-Kugel als Empfänger neben der Built-in-Kontrollkugel (Muster `HE_DUMP_MATOCCLUDER`) |

Die Pixel bleiben damit unbewiesen, bis jemand auf echter Hardware läuft. Das ist dieselbe offene
Schuld wie bei den übrigen GI-Blind-Ports.

## 5. Stand nach Schritt 2 (02.10.2026)

### 5.1 Die Lücke, präzise benannt

Betroffen ist **nur der Graph-Material-Pfad** (`heLitP` → `heGIIrradianceAt`,
`MaterialShaderLibrary.cpp:291-330`, Zweig `:625`). Der eingebaute PBR-Shader hat DDGI auf allen
fünf Backends. Pro Backend fehlen zwei Dinge: das Gate `Lighting::giProbe.y` samt Grid-Werten im
Material-Fill und die Probe-Atlanten auf den Material-Slots t17/t18 (Sampler s1/s3).

| Backend | Gate im Fill | Atlanten auf t17/t18 | Stand |
|---|---|---|---|
| GL | ja | ja | unverändert (nur auf Helfer umgestellt) |
| Metal | ja | ja | unverändert (nur auf Helfer umgestellt) |
| D3D12 | **jetzt ja** (`fillMatLight`) | **jetzt ja** (Vorlage, Slots 12/13) | **in diesem Schritt behoben** |
| D3D11 | **Schritt 3: ja** (`fillMatLight`) | **Schritt 3: ja** (Bind + Restore pro Draw) | in Schritt 3 behoben, s. §6 |
| Vulkan | **Schritt 4: ja** (Material-Fill in `DrawScene`) | **Schritt 4: ja** (DSL 21 Bindings, Write pro Draw) | in Schritt 4 behoben, s. §7 |

### 5.2 Was geändert wurde

- **Gemeinsamer Helfer** `HE::FillMaterialGIProbe(Lighting&, origin, spacing, counts, perRow,
  intensity, atlasesBound)` in `LightPacking.h/.cpp` (hinter `FillMaterialWind`). Er schreibt
  `giGridOrigin`, `giGridCounts` und `giProbe`. Das Gate ist 1 nur bei `atlasesBound`.
- **GL** (`OpenGLRenderer.cpp`, Material-Fill) und **Metal** (`MetalRenderer.mm`, Material-Fill)
  rufen den Helfer statt der Inline-Zeilen. Es sind dieselben Werte und dasselbe Gate, also eine
  reine Refaktorierung.
- **D3D12** (`D3D12Renderer.cpp`):
  - `fillMatLight` ruft den Helfer mit `giActive && giIrrTex && giVisTex`. `giActive` ist dasselbe
    `giShadingActive`, das auch der eingebaute Shader bekommt.
  - Neuer `writeMatGiProbeSlots()`: schreibt die Atlas-SRVs (RGBA16F / RG16F) in die Slots
    `kSlotGIIrradiance`/`kSlotGIVisibility` der CPU-Vorlage `m_matSrvStaging`, sonst Null-Views.
    Aufgerufen wird er am Ende von `ensureGiProbeAtlas`, in `retireGiProbeAtlas` (Refit → Null)
    und in `destroyGiTargets`.
  - Warum keine Synchronisation nötig ist: Die Vorlage wird bei jedem Material-Draw zur
    **Aufzeichnungszeit** per `CopyDescriptorsSimple` in den Ring kopiert. Bereits aufgezeichnete
    Blöcke behalten also ihre Views.
  - Ressourcenzustand: Die Atlanten stehen in `PIXEL_SHADER_RESOURCE`, sobald `giShadingActive`
    gilt (Barrier am Ende von `dispatchGiProbeUpdate`). Der einzige Material-Draw (Geometrie-Pass)
    wird danach aufgezeichnet.
  - Die Root-Signatur bleibt unverändert: t17/t18 und s1/s3 (Linear-Clamp) waren schon deklariert,
    der WARP-PSO-Test bleibt gültig.
  - Veraltete Kommentare zu „Slots 9..16 null / D3D11-parity job" sind korrigiert, ebenso der
    Kopf von `D3D12MaterialRootSignature.h`.
- **Shader-Parität geprüft (statisch):** `heGIIrradianceAt` (Preamble) und das D3D12-HLSL
  `sampleDDGIIrradiance` (`D3D12Renderer.cpp`, ab „float3 sampleDDGIIrradiance") stimmen Zeile für
  Zeile überein: Tile-Position, Oktaeder-Kodierung, UV-Formel, kein y-Flip, Chebyshev. Die Atlanten
  haben eine Mip-Stufe, `texture()` und `SampleLevel(…, 0)` sind also gleichwertig.
- **Test:** `tests/test_material_graph.cpp` mit dem Fall „FillMaterialGIProbe: the built-in
  shaders' grid, gated on bound atlases". Er prüft Werte, Gate 1/0 und dass Nachbarfelder
  unberührt bleiben.

### 5.3 Was offen bleibt (Umfang für die Folgeschritte)
- **D3D11 (Schritt 3): erledigt, s. §6.** Ursprünglicher Plan:
- **D3D11 (Schritt 3, ≈ 1 Tag):**
  - `fillMatLight` (`D3D11Renderer.cpp:5724`) auf den Helfer umstellen.
  - Im Material-Draw (Bindungen ab `:6300`, `BindLandscapeWeights` `:6329`): t17 = `giIrrSRV`,
    t18 = `giVisSRV` (`:2937`), s1/s3 = `giLinearClamp` (`:2903`).
  - Nach dem Draw zurücklegen, was der eingebaute Pass dort erwartet: t17 lokaler Schattenatlas
    (`:6066`), t18 Cluster-Lichter (`:5644`), s1 `pointSampler` (`:6068`), s3 `shadowSampler`
    (`:6061`). Das gehört als zweites Paar neben `RestoreAfterMaterialDraw` (`:6386`) in
    `D3D11MaterialBindings.h`, samt WARP-Test.
  - Achtung: PR #75 (Thema 117) baut den D3D11-Material-Draw und die Bindings gerade um.
- **Vulkan (Schritt 4, ≈ 1-1,5 Tage):**
  - Fill (`:6378`) auf den Helfer umstellen.
  - DSL `:2318-2353` um die Bindings 15/16/17/18/32/33 erweitern, dazu Pool, Per-Draw-Writes und
    Weiß-/Null-Cube-Fallbacks (s. §2.2).
  - Damit wird zugleich die spec-widrige Layout-Lücke geschlossen.
- **Nicht verifiziert:**
  - Der D3D12-Code ist lokal nicht kompilierbar und hängt an der Windows-CI.
  - Die Bildparität Graph-Material gegen Built-in bei GI an ist auf D3D12 ohne Windows-Hardware
    nicht prüfbar (Zeuge wie in §4).

## 6. Stand nach Schritt 3: D3D11 (02.10.2026)

### 6.1 Was geändert wurde

- **Fill:** `fillMatLight` (`D3D11Renderer.cpp`) ruft `HE::FillMaterialGIProbe` mit
  `giActive && p.giIrrSRV && p.giVisSRV`, also exakt die D3D12-Form. `giActive` ist das
  `giShadingActive` des eingebauten Shaders.
- **Bind + Restore als zweites Paar in `D3D11MaterialBindings.h`**, neben dem t14/s0-Paar:
  - `kGIIrradianceSrvSlot/SamplerSlot` = t17/s1, `kGIVisibilitySrvSlot/SamplerSlot` = t18/s3
    (gegen `kHlslMaterialPins`).
  - `BindDDGIAtlases(ctx, irr, vis, linearClamp)` vor jedem Material-Draw. Bei aktivem GI die
    echten Atlanten, sonst der weiße Dummy. Gebunden wird **immer**, nicht nur bei offenem Gate:
    Der Material-PS deklariert t17/t18 als `Texture2D`. Bliebe dort das Texture2DArray bzw. der
    StructuredBuffer des eingebauten Passes liegen, wäre das auch hinter einem 0-Gate ein
    Dimensions-Mismatch (Debug-Layer-Meldung pro Draw). Der Sampler ist `m_matWeightSampler`:
    dieselbe Linear-Clamp-Beschreibung wie `giLinearClamp`, existiert aber auch ohne GI-Pfad.
  - `RestoreBuiltinGISlots(ctx, BuiltinGISlots)` nach dem Draw: t17 = `localShadowSrv_` (null ohne
    Lokal-Atlas), t18 = `clusterLightSRV` (null wenn nicht clustered), s1 = `pointSampler`,
    s3 = `shadowSampler`. t19/t20 (Cluster-Grid/-Indizes) fasst der Material-Draw nicht an.
- **PR #75 (Thema 117) kollidiert bei den Registern nicht:** Die Material-Cluster-Buffer liegen dort
  auf t24-26. In `test_material_graph.cpp` und im D3D11-Material-Draw ist beim Zusammenführen aber
  mit Textkonflikten zu rechnen.

### 6.2 Test (Windows-CI, WARP)

`test_material_graph.cpp`, Fall „D3D11: a graph material draw reads the DDGI atlases on t17/t18
(s1/s3) …". Ein echter Lit-Graph (weiße Basis) durch FXC, HeLighting über `FillMaterialGIProbe`.
Zwei Probes übereinander (Grid 1×1×2), Irradiance-Kacheln rot/grün, Visibility Kachel 0 offen,
Kachel 1 verdeckt. Erwartet:

| Fall | Pixel |
|---|---|
| Atlanten gebunden, Gate an | (128,128,0), 50/50-Mischung |
| Gate aus | schwarz (Ambient 0, keine Lichter) |
| t18 leer | ≈ (12,243,0), beide Probes lesen „verdeckt" |
| t17 leer | schwarz |
| nach Restore | t17/t18/s1/s3 wieder Array, Buffer, Point, Shadow |

Die Werte sind von Hand gerechnet, die Herleitung steht im Kommentar über dem Testfall.
Lokal (macOS) läuft der Fall nicht, er hängt an der Windows-CI.

### 6.3 Was offen bleibt

- **Vulkan (Schritt 4)** wie in §5.3.
- Bildparität Graph-Material gegen Built-in bei GI an: auf D3D11 ebenso unbewiesen wie auf D3D12,
  braucht Windows-Hardware (Zeuge wie in §4).

## 7. Stand nach Schritt 4: Vulkan (02.10.2026)

### 7.1 Was geändert wurde (`VulkanRenderer.cpp/.h`, Commit `a20eb0dd`)

- **Fill:** Der Material-Fill in `DrawScene` ruft `HE::FillMaterialGIProbe` mit demselben Grid,
  das der Frame-UBO an `scene.frag` gibt. Gate und Descriptor-Wahl für 17/18 hängen an **einem**
  Prädikat: `matGiProbes = m_giRanThisFrame && m_giProbeGridBuilt && beide Atlas-Views`. Das Gate
  kann also nie offen stehen, während dort Weiß liegt.
- **DSL 15 → 21** (`k_matSetBindings`): neu sind 15/16/17/18/32/33. Der Pool hat 16 Sampler pro
  Set, jeder Draw schreibt 21 Descriptoren. Das Layout deklariert damit jedes Binding, das das
  Preamble-SPIR-V statisch nutzt. Die spec-widrige Lücke aus §2.2 ist zu, bis auf 14 (s. u.).
  - 17/18: die Atlanten in `GENERAL`, mit Linear-Clamp (`m_ssaoSampler`, wie Szenen-Bindings 5/6),
    sonst Weiß.
  - 15 `heSkyEnv`: neuer weißer 1×1-Cube (`m_whiteCubeImage/View`, sechs Layer,
    CUBE_COMPATIBLE). Das 2D-Weiß kann keine Cube-View tragen.
  - 16/32/33: Weiß. Ihre Gates (`fog.w`, `giRefl.z`, `cloudShadowB.x`) bleiben auf Vulkan 0.
- **Shader-Parität (statisch):** `heGIIrradianceAt` und `scene.frag` `sampleDDGIIrradiance`
  stimmen Zeile für Zeile überein: Tile-Mathe, Oktaeder, UV, kein y-Flip, Chebyshev.

### 7.2 Geprüft auf NN-WS03 (RTX 4070, Vulkan-Validation an, Release)

Pre = Elternstand `c8268cd4`, post = `a20eb0dd`. Beide kommen aus demselben Baum, nur die zwei
Vulkan-Dateien unterscheiden sich. Jede Aufnahme hat eine frische APPDATA, `HE_SKY_TIME=10` und
40 Frames.

- **Build + `he_tests`:** 4130/4130 Fälle grün.
- **Validation:**
  - Bemaltes Terrain (`HE_DUMP_LANDSCAPELAYERS=1`): pre meldet die Bindings 14,15,16,17,18,32,33,
    post nur noch 14.
  - Graph-Kugel (`HE_DUMP_MATERIALTEST=1`): pre meldet 15–18/32/33 (17 Fehler), post **keinen**.
  - Probe-Grid-Refit mitten im Lauf (`HE_DUMP_GIREFIT=1`, Grid 15×4×15 → 22×4×11, Atlanten neu):
    post meldet nur Binding 14, keine anderen Fehler. Der Lebensdauer-Pfad der 17/18-Writes ist damit
    einmal durchlaufen.
  - Keine neuen Fehlerarten. Die Shutdown-Leak-Liste ist pre wie post dieselbe (10 Objekte,
    ImGui-förmig).
- **Pixel** (bemaltes Terrain = Graph-Material, Draufsicht; mittlere |Δ| in 8-Bit-Stufen):

| Vergleich | mittlere \|Δ\| | Pixel > 2 |
|---|---|---|
| GI aus, pre vs post | 0,01 | 0,0 % |
| GI an, post vs post (Rauschen) | 0,25 | 0,8 % |
| GI an, pre vs post | **7,42** | **50,9 %** (Mittel R 187 → 195) |
| Kugel, GI aus, pre vs post | 0,03 | 0,0 % |
| Kugel, GI an, pre vs post | 1,17 | 11,1 % |

  Bei GI an hebt der Fix die dunklen GI-Schattenstreifen des Terrains weich an, mit dem
  Probe-Kachelmuster (Spacing 8,7 m). Bei GI aus ist er unsichtbar.

### 7.3 Was offen bleibt

- **Binding 14 (`heLandscapeWeights`)** fehlt weiter im Vulkan-Material-Layout. Der Node-Codegen
  deklariert es nur bei Landscape-Layer-Knoten. Das ist nicht DDGI, sondern ein eigener Punkt
  (bemalte Terrains auf Vulkan). **Achtung Limit:** Die Fragment-Stage des Material-Layouts hat
  jetzt genau 16 Combined-Image-Sampler (b2, b4–7, b10–13, b15–18, b31–33). Das ist das
  Spec-Minimum von `maxPerStageDescriptorSamplers`/`…SampledImages`. Binding 14 dazu ergibt 17;
  wer das nachzieht, muss gegen das Geräte-Limit prüfen. Das Landscape-SPIR-V nutzt die 17 heute
  schon statisch.
- **Kein automatischer Vulkan-Test.** Die Belege in §7.2 sind manuelle Hardware-Läufe. Anders als
  bei D3D11/D3D12 (WARP) gibt es in der CI kein Vulkan-Gerät (s. Thema 122).
- Nebenbefund 1 (Forward-SSR für Graph-Materialien tot auf Vulkan) unverändert, gehört zu Thema 126.
- Parität Graph-Material gegen Built-in Pixel für Pixel ist nicht gemessen. Belegt ist nur:
  Graph-Material bekommt jetzt Probe-Licht, und GI aus bleibt bitgleich.
