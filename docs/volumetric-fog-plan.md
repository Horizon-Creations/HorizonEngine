# Volumetrischer Nebel — Entwurf (Thema 225, Schritt 1)

Stand 19.10.2026, Zweig `claude/echtes-volumetrisches-nebel-statt-hintergrund-blend` auf `main` (1c1fd193).
Nur Entwurf, kein Engine-Code geändert. Zeilennummern gelten für 1c1fd193 und stammen aus
`grep -n`; die Backend-Zeilen der Pass-Reihenfolge (§3) hat ein Survey-Agent ermittelt, sie sind
Einstiegspunkte, keine Verträge.

## 0. Kurzfassung der Entscheidungen

| # | Entscheidung | Kurzbegründung |
|---|---|---|
| E1 | **Froxel-Gitter** (kameraausgerichtetes 3D-Gitter, exponentielle Tiefenscheiben) statt reinem Per-Pixel-Raymarching | Kosten unabhängig von Auflösung und Overdraw; Licht/Schatten nur einmal pro Froxel; Transluzente lesen dasselbe Volumen |
| E2 | Drei Pässe: **Inject** (Dichte + Licht pro Froxel, mit temporaler Reprojektion) → **Integrate** (Front-to-Back-Scan pro Spalte) → **Apply** (Vollbild-Komposit) | Standardaufbau (Wronski 2014 / Hillaire 2015), passt zu jedem Backend |
| E3 | Opake Flächen + Himmel bekommen den Nebel im **Apply-Pass an der Naht opak→transluzent**, nicht mehr im Shader | Ein neuer Shader pro Backend statt zehn neuer Lesestellen; kein Sampler-Budget-Problem (§5.4) |
| E4 | Transluzente Graph-Materialien (inkl. Wasser) lesen das Volumen **im Shader** über `heApplyFogT` (Preamble, `texelFetch`, Binding 34) | Sie liegen hinter der Naht; `texelFetch` braucht kein Sampler-Register (D3D ist bei 16/16) |
| E5 | Hinter der Volumenreichweite `D` bleibt die **heutige analytische Formel als Fernfeld**, nahtlos angesetzt | Horizont-Look bleibt; Volumen braucht nur die Nähe aufzulösen |
| E6 | **Modus `fogMode` 0 = Analytisch (heute, pixelgleich) / 1 = Volumetrisch**; Software und Backends ohne Fähigkeit bleiben auf 0 | Alter Szenenstand lädt unverändert; harter Fallback |
| E7 | Dichte = heutige Höhenformel × Rauschen aus dem **vorhandenen `SkyNoise3D`** + Bodennebelbank + Niederschlags-Dunst | Weather-Presets wirken weiter; keine neue Rauschtextur |
| E8 | Schatten: **1 Tap CSM** (vorhandene Kaskaden, `heCsmShadow`-Mathe), Wolkenschatten wo vorhanden, **Local-Atlas 1 Tap** für die 8 Fensterlichter | Bestehende Maps, keine neuen Shadow-Pässe |
| E9 | Temporale Reprojektion **im Volumen**, eigene HDR-History, Blendgewicht aus Zeitkonstante (τ = 40 ms) | TAA läuft nach dem Tonemap auf LDR und hilft dem Volumen nicht |
| E10 | Compute auf Metal/Vulkan/D3D11/D3D12; **OpenGL (alle Versionen) als Fragment-Pfad** pro Scheibe | macOS-GL ist 4.1 ohne Compute; ein GL-Pfad statt zwei |

## 1. Ist-Zustand (verifiziert per grep)

- **Formel:** `applyFog()` GL `OpenGLRenderer.cpp:272`, Metal `MetalRenderer.mm:652`, Preamble `heApplyFog`
  `MaterialShaderLibrary.cpp:365`. Optische Tiefe `density · dist · exp(-falloff · cam.y) · (1-e^-k)/k`,
  `k = falloff · ray.y`; Farbe = Himmel in Blickrichtung (`skyColor()` bzw. `heSkyEnv`), `mix(color, fogCol, 1-e^-optical)`.
  Kein Licht, kein Schatten, keine Dichtevariation.
- **Lesestellen heute:** GL Szene/Skinned/Instanced (eigene Uniforms `m_uFogDensity`, `m_uSkinnedFogDensity`,
  `m_uInstFogDensity`), Metal Szene-MSL (`:947`), D3D11 `uFog` (`:423`, Fill `:6420/:6554`), D3D12 (`:456`,
  `:10380/:10513`), Vulkan `scene.frag:94` (Fill `:7947/:8092`), Preamble `heApplyFog` (Forward + Deferred-Resolve,
  `MaterialGraph.cpp:1944` für lit Graph-Materialien jeder Blend-Art), Transmittanz-Spiegel der deferred
  Reflexions-Addition `MaterialShaderLibrary.cpp:1857`. Der Himmel selbst wird nicht vernebelt.
- **Parameter:** `EnvironmentComponent::fogDensity` (0) / `fogHeightFalloff` (0.1), serialisiert über
  `HE_ENV_FIELDS_FLOAT` (`EngineApi.h:1486`, Serializer `SceneSerializer.cpp:538`), Push `EnvironmentPush.cpp:30`,
  Inspector `InspectorPanel.cpp:1205` ("Atmospheric Fog", Slider 0..0.15).
- **Wetter:** `WeatherSystem.cpp:13-26` treibt nur `fogDensity` (Foggy 0.08, Rain 0.03, Storm 0.04, Snow 0.05,
  Overcast 0.01) über `drive()` mit `lastFog`-Sentinel; `fogHeightFalloff` treibt niemand.
- **Software-Backend** zeichnet kein 3D (`SoftwareRenderer.cpp`, Capabilities alle false) — nichts zu tun.

## 2. Datenmodell

### 2.1 Neue Felder in `EnvironmentComponent` / `EnvironmentSettings`

Alle über die X-Listen in `EngineApi.h` (Serialisierung, Skript-API, Display-Name in einem). Fehlender Schlüssel
= Default, deshalb lädt jede alte Szene mit `fogMode = 0` exakt wie heute. Defaults der übrigen Felder sind
„guter Volumen-Look“, sie wirken nur in Modus 1.

| Feld | Liste | Typ / Default | Bedeutung |
|---|---|---|---|
| `fogMode` | INT | 0 | 0 Analytisch (heute), 1 Volumetrisch |
| `fogQuality` | INT | 1 | 0 Low, 1 Medium, 2 High → Gittergröße (§4.1) |
| `fogVolumeDistance` | FLOAT | 96 m | Reichweite `D` des Volumens (Planar-Tiefe), geklemmt auf 16..min(512, CSM-Distanz) |
| `fogAlbedo` | COLOR | (1,1,1) | Streu-Albedo/Tönung (σs = albedo · σt) |
| `fogAnisotropy` | FLOAT | 0.5 | Henyey-Greenstein `g` (−0.9..0.9) |
| `fogNoiseAmount` | FLOAT | 0.5 | 0 = reine Höhenformel … 1 = volle Modulation |
| `fogNoiseScale` | FLOAT | 40 m | Weltgröße einer Rauschperiode |
| `fogBankDensity` | FLOAT | 0 | Bodennebelbank, Extinktion 1/m |
| `fogBankBaseY` | FLOAT | 0 m | Welt-Y Unterkante der Bank |
| `fogBankThickness` | FLOAT | 4 m | Dicke der Bank (Oberkante weich, rausch-erodiert) |
| `fogLocalLightScatter` | FLOAT | 1 | Faktor für Punkt-/Spot-In-Scattering (0 = aus) |

`fogDensity`/`fogHeightFalloff` behalten Einheit und Semantik (1/m bei Welt-Y 0; Abfall pro Meter Höhe).

**Mitzuziehen** (sonst rot oder still wirkungslos): `HE_ENV_FIELDS_*`-Zeilen; `HcNodeDocs.cpp` je neue Row
(`test_hc_node_docs`, nur im vollen ctest); `EnvironmentSettings` + `makeEnvironmentSettings`
(`EnvironmentPush.cpp`); Inspector-Zeilen unter „Atmospheric Fog“ + `EditorHelp.cpp`
(`scripts/editor_help_audit.py --check`); Serializer-Roundtrip in `tests/test_scene_serializer.cpp` plus ein Test,
der eine Szene **ohne** die neuen Schlüssel lädt.

### 2.2 `FogVolumeData` in `RenderWorld.h` (vom Extractor gefüllt)

```cpp
struct FogVolumeData {
    bool      enabled  = false;   // fogMode==1 && density/bank > 0 && Backend kann es && Kamera nicht unter Wasser
    int       quality  = 1;
    glm::ivec3 grid    {160, 90, 64};   // aus quality + Seitenverhältnis (§4.1)
    float     distance = 96.0f;  // D
    float     sliceC   = 2.0f;   // Verteilungskonstante c (§4.2)
    float     density, heightFalloff, noiseAmount, invNoiseScale;
    float     bankDensity, bankBaseY, bankThickness;
    float     precipExtinction;  // aus rain/snowAmount (§6.3)
    glm::vec3 albedo;  float anisotropy;
    float     localLightScatter;
    float     flash;             // Blitz hellt Nebel auf
};
```

`enabled == false` heißt: Backend fährt exakt den heutigen Pfad (E6). Zustand über Frames (History, Jitter-Index,
Rausch-Offset) hält das **Backend** (wie die TAA-History), nicht der Extractor.

### 2.3 Gemeinsamer Host-Header `HorizonRendering/FogVolume.h` (header-only, `namespace HE`)

Vorbild `TemporalAA.h` / `LightPacking.h`. Enthält, was alle Backends identisch brauchen:

- `fogGridFor(quality, width, height)` → `ivec3`.
- `fogSliceToDepth(t, D, c)` / `fogDepthToSlice(z, D, c)` (§4.2).
- `fogOpticalSegment(camY, dirY, a, b, density, falloff)` — analytisches Integral der Höhenformel von Strahl-Distanz
  `a` bis `b` (Grenzfall `k→0`). Die heutige Formel ist `fogOpticalSegment(…, 0, d, …)`.
- `fogHistoryAlpha(dtSeconds)` = `1 − exp(−dt / 0.040)`.
- `fogJitter(frameIndex)` — R1-Folge (0..1) über 16 Werte, unabhängig von `taaJitter`.
- `struct FogVolumeConstants` (std140, Layout §2.4) + `FillFogVolumeConstants(...)`.
- `FogIntegrateColumnCPU(...)` — CPU-Referenz des Integrate-Passes für Tests (§9).

### 2.4 Konstanten des Volumen-Passes (`FogVolumeConstants`, std140, 288 Byte)

| Offset | Feld | Inhalt |
|---|---|---|
| 0 | `mat4 invViewProj` | unjittered, Clip-Konvention des Backends vorgebacken (Froxel → Welt) |
| 64 | `mat4 prevViewProj` | unjittered Vor-Frame (Reprojektion); Backend-Felder `m_taaPrevViewProj` & Co. existieren überall |
| 128 | `vec4 camPos` | xyz Kamera, w = `D` |
| 144 | `vec4 camFwd` | xyz Blickachse (Planar-Tiefe wie CSM-Kaskadenwahl), w = `c` |
| 160 | `vec4 grid` | xyz Gittergröße, w = 1/z-Anzahl |
| 176 | `vec4 density` | x density, y heightFalloff, z noiseAmount, w invNoiseScale |
| 192 | `vec4 bank` | x bankDensity, y bankBaseY, z bankThickness, w precipExtinction |
| 208 | `vec4 noise` | xyz Rausch-Offset (Wind-akkumuliert, §6.2), w = Zeit (Rausch-Evolution) |
| 224 | `vec4 scatter` | rgb albedo, w anisotropy g |
| 240 | `vec4 temporal` | x Jitter 0..1, y History-α, z History gültig (0 = Reset), w Frame-Index |
| 256 | `vec4 misc` | x localLightScatter, y flash, z Sonnen-Anteil der Nebelfarbe (Konstante 0.5, §5.2), w Unterwasser-Gate |
| 272 | `vec4 pad` | reserviert |

Licht, CSM-Matrizen, Local-Atlas-Matrizen, Wolkenschatten-Region kommen **nicht** doppelt hierher: Der
Inject-Pass bindet zusätzlich den ohnehin gefüllten `HeLighting`-UBO (`kLightingPreamble`, Binding 0) und ruft
dieselben Funktionen wie die Flächen (`heCsmShadow`, `heLocalShadowFactor`, `heCloudShadowFactor`). Nebelschatten
und Flächenschatten liegen damit per Konstruktion deckungsgleich.

### 2.5 Erweiterung `HeLighting` / `MaterialShaderLibrary::Lighting` (append-only, v3.6)

```glsl
vec4 fogVol;      // x = 1 → Volumen-Modus (opake heApplyFog = no-op), y = D, z = c, w = 1 → heFogVolume gebunden
vec4 fogVolGrid;  // xyz Gittergröße, w frei
mat4 fogViewProj; // unjittered, Konvention vorgebacken (Welt → Froxel-uv)
```

Am Ende des UBOs anhängen wie v3.1–v3.5; jede Fill-Site, die heute `fog` füllt, füllt auch diese (Metal
`FillMaterialLighting`, GL `fillMatLight`, D3D11/D3D12/Vulkan `lit.fog`-Stellen).

### 2.6 Texturen

| Name | Typ / Format | Größe (Medium) | Inhalt |
|---|---|---|---|
| `fogInject[2]` | 3D RGBA16F, Ping-Pong | 160×90×64 | rgb = σs·L_in (Streustrahldichte pro m), a = σt (1/m); temporal gefiltert |
| `fogIntegrated` | 3D RGBA16F | 160×90×64 | rgb = akkumuliertes In-Scatter L(0→Scheibe), a = Transmittanz T |

Speicher Medium: 3 × 160·90·64 · 8 B = 22 MB. Low 4 MB, High 48 MB (§4.1). Lesen per Hardware-Trilinear im
Apply-Pass, per `texelFetch`-Trilinear (8 Fetches) in `heApplyFogT`.

## 3. Pässe und Einbaustellen

```
Shadow (CSM + Local-Atlas) ▸ Cloud-Shadow (Metal/GL) ▸ SkyEnv-Cube
  ▸ [neu] FogInject  (Dichte, Licht, Schatten, Reprojektion)        ← nach allen Shadow-Maps
  ▸ [neu] FogIntegrate
  ▸ GI / SSAO / G-Buffer / Resolve / opake Forward-Draws / Himmel
  ▸ [neu] FogApply   (Vollbild, Tiefe lesen, Blend ONE, SRC_ALPHA)  ← Naht opak+Himmel → transluzent
  ▸ Transluzente (heApplyFogT liest fogIntegrated) ▸ Partikel ▸ PostFX (Unterwasser, Bloom, Tonemap, TAA …)
```

Inject/Integrate hängen nicht von der Szenentiefe ab, nur von Shadow-Maps und Licht — sie können früh laufen
(Metal: async Compute ist eine spätere Option).

**Einbaustelle FogApply pro Backend** (Survey, Stand 1c1fd193):

| Backend | Stelle | Was zu tun ist |
|---|---|---|
| Metal | `EncodeScene`: Himmel `drawSky`-Lambda ~14081, Transparente ~14744, beides im `hdrPass`-Encoder (Forward **und** Deferred-Schwanz) | Encoder nach dem Himmel beenden, FogApply als eigener Render-Pass auf `m_hdrColor` (liest `m_hdrDepth`, ShaderRead vorhanden), danach Encoder mit Load für die Transparenten neu öffnen. Nur wenn `enabled`. |
| OpenGL | `DrawSkyFullscreen` ~12715 → Transparente ~12797 | FogApply zwischen beiden, in ein **Nur-Farbe-FBO** auf `m_hdrColor` (Tiefe als Textur gebunden = sonst Feedback-Loop), danach `m_hdrFBO` zurück |
| Vulkan | `DrawScene`: Himmel zuerst ~7827, Deferred ~8724-8782, Transparente am Ende, alles in `m_postFxSceneRP` | Render-Pass vor den Transparenten teilen; Tiefe → SHADER_READ wie im Unterwasser-Block (~877-890); FogApply-RP; Szenen-RP mit LOAD fortsetzen |
| D3D11 | Decals ~7607 → Transparente ~7616 | FogApply dazwischen; DSV lösen bzw. **read-only DSV**, damit die Transparenten weiter tiefentesten |
| D3D12 | Decals ~11662 → Transparente | Übergang DEPTH_WRITE → `DEPTH_READ | PIXEL_SHADER_RESOURCE`, FogApply, Transparente mit read-only DSV |

Vorbild für Konstanten, Tiefenrekonstruktion und Blend ist der Unterwasser-Nebel-Pass (GL
`RenderUnderwaterCaustics` ~5713, Metal `EncodeUnderwaterCaustics` ~12222, Vulkan ~843-960, D3D11 ~7766,
D3D12 `runPostFX` ~2530, HLSL `D3D_Shared/HlslSources.h:440`). **Tiefenrekonstruktion immer über die
`depthParams`-Zeile des jeweiligen Backends**, nie mit einer Near-Konstante (Lehre aus Thema 185).

## 4. Gitter

### 4.1 Größen pro Qualität

| Qualität | X | Y | Z-Scheiben | Froxel | Speicher (3 Volumen) |
|---|---|---|---|---|---|
| Low | 96 | ⌈96/Seitenv.⌉ (54) | 32 | 166 k | 4 MB |
| Medium | 160 | 90 | 64 | 922 k | 22 MB |
| High | 192 | 108 | 96 | 1,99 M | 48 MB |

X fest pro Stufe, Y aus dem Seitenverhältnis der **Render**-Auflösung (nicht der Fenstergröße; Render-Scale
ändert das Gitter nicht). Begründung: Kosten unabhängig von Retina/4K; das Baseline-Fenster 2840×1528 hätte mit
festen 8-px-Kacheln 355×191 Spalten (4,3 M Froxel bei 64 Scheiben).

### 4.2 Tiefenverteilung

Planar-Tiefe `z` (entlang `camFwd`, wie die CSM-Kaskadenwahl), exponentiell mit linearem Anfang:

```
z(t) = c · ((1 + D/c)^t − 1),     t ∈ [0,1] (Scheibengrenzen t = k/N)
t(z) = ln(1 + z/c) / ln(1 + D/c)
```

`c = 2 m`, `D = 96 m`, N = 64 → erste Scheibe 0,13 m, letzte 5,8 m dick. Der Scheibenindex kommt **immer** aus
rekonstruierter Weltposition → Planar-Tiefe, nie aus dem rohen Tiefenpuffer. `D` wird auf die CSM-Distanz
(Projekt-Default 250 m) geklemmt, damit es im Volumen keinen Bereich ohne Schattenkarte gibt.

## 5. Inject: Dichte und Licht pro Froxel

Weltposition = Froxel-Mitte in xy, in z um `temporal.x` (Jitter) innerhalb der Scheibe verschoben.

### 5.1 Dichte (σt, 1/m)

```
base   = density · exp(−heightFalloff · y)                                  // heutige Formel, unverändert
n      = SkyNoise3D.G-fBm(p · invNoiseScale + noiseOffset), 2 Oktaven → [0,1]
height = base · max(0, 1 + noiseAmount · (2n − 1))
bank   = bankDensity · step(bankBaseY, y) · (1 − smoothstep(top − 0.5·bankThickness·(0.5+n), top, y)),
         top = bankBaseY + bankThickness
σt     = height + bank + precipExtinction
```

`SkyNoise3D` (RG16, hash-gepinnt in `test_culling.cpp`) ist auf allen vier GPU-Backends bereits eine 3D-Textur
(Metal `MTLTextureType3D` ~7636, GL `m_noiseTex` ~4413, Vulkan R16G16 ~9173, D3D11 `noiseTex3D` ~5470, D3D12
`skyNoiseTex12` ~2698). Wird nur **gelesen**, die Bytes bleiben unberührt.

### 5.2 In-Scattering (Strahldichte, die pro Meter in Blickrichtung gestreut wird)

```
fogCol = Himmel in Richtung der Froxel-Spalte (heSkyEnv; dieselbe Farbe wie die heutige Formel)
sunVis = heCsmShadow(p) · heCloudShadowFactor(p)          // 1 Tap; außerhalb CSM = 1
S = σt · fogCol · mix(1, sunVis, kSunShare · seamFade)            // „Umgebung“, nahtfähig zum Fernfeld
  + σs · sunColor · HG(g, dot(V, L)) · sunVis · seamFade          // Sonnen-/Mondschächte
  + σs · Σ_i lightColor_i · att_i · cone_i · HG(g, dot(V, L_i)) · heLocalShadow_i · localLightScatter
  + σs · ambient · flash                                         // Blitz
σs = albedo · σt
```

- **`fogCol` ist immer `heSkyEnv`** (128²-Cube), in Inject, Apply und `heApplyFogT` — die Himmelsbibliothek
  (`kSkyFS`/`skyColor()`) wird in keinen neuen Shader gezogen. Heute rechnen die eingebauten Shader die Nebelfarbe
  prozedural pro Pixel (GL `:272`, Metal `:652`), Graph-Materialien aus dem Cube. Für eingebaut schattierte Flächen
  ist Modus 1 deshalb eine bewusste, kleine Farbänderung am Horizont/um die Sonne.
- **Sonne/Mond:** der Richtungslicht-Eintrag des RenderWorld (Tag Sonne, Nacht Mond), Farbe/Intensität wie die
  Flächen. Wolkenschatten nur Metal/GL (Vulkan/D3D binden Weiß bzw. Gate 0 — dieselbe Lücke wie bei den Flächen).
- **`kSunShare = 0.5`:** Anteil der Himmels-Nebelfarbe, der im Schatten verschwindet. Ohne ihn wären
  Volumenschatten nur „weniger hell“, nie dunkler als der Horizont. Konstante in `FogVolume.h`, kein UI.
- **`seamFade`** = `1 − smoothstep(0.8·D, D, z)`: Sonnenterm und Schattenanteil fehlen im Fernfeld, also laufen
  beide vor der Naht aus (§7).
- **Lokale Lichter:** die 8 Fensterlichter aus `HeLighting` (`lightPos/Dir/Color/Params`), Abschwächung exakt wie
  `heLitP`, Schatten 1 Tap aus dem Local-Atlas (`lightParams.y` = Layer+1). Das Cluster-Gitter (16×9×24) ist eine
  spätere Optimierung (Phase 2), nicht Teil von v1.
- **Phase:** Henyey-Greenstein, normiert (1/4π); `g` aus `fogAnisotropy`.

### 5.3 Temporale Reprojektion (im selben Pass)

Weltposition → `prevViewProj` → Vor-Frame-uvw (`t(z)` mit derselben Verteilung) → History trilinear aus
`fogInject[prev]` → `out = mix(history, new, α)`, `α = fogHistoryAlpha(dt)` (200 FPS ≈ 0,12, 60 FPS ≈ 0,34;
konstante ~40-ms-Latenz statt fester Frameanzahl, vgl. die 80-ms-Warnung in `sky-weather-performance-plan.md`
§4 Stufe 2b). Reset (`temporal.z = 0` → α = 1): uvw außerhalb [0,1], erster Frame, Gitter-/Qualitätswechsel,
Kamerasprung > 10 m oder FOV-Änderung. Jitter: `fogJitter(frame)` in z, kein xy-Jitter.

### 5.4 Warum Apply statt Lesen im Shader (E3/E4)

Die Alternative „jede `applyFog`-Stelle liest das Volumen“ hätte zehn Lesestellen (GL ×3 Programme, Metal, D3D11,
D3D12, Vulkan, Preamble, Reflexions-Spiegel, **neu** der Himmel) mit 8-Fetch-Trilinear unter Forward-Overdraw.
Außerdem ist das D3D-Material-Sampler-Budget voll (SM 5.0, 16/16, `MaterialShaderLibrary.cpp:2655-2700`), und
GL 4.1 auf dem Mac hat 16 Textureinheiten pro Fragment-Stufe. Der Apply-Pass ist ein eigener Shader mit eigenen
Bindings, deckt den Himmel automatisch ab (Tiefe = fern) und braucht die Naht opak→transluzent, die es auf
GL/D3D11/D3D12 schon gibt (TAA-Velocity/Decals). Teilen muss man nur Metal (`hdrPass`) und Vulkan
(`m_postFxSceneRP`).

## 6. Integrate und Apply

### 6.1 Integrate (pro Spalte, Front-to-Back, energieerhaltend nach Hillaire)

```
L = 0, T = 1
für k = 0..N−1:  dz = Strahl-Länge der Scheibe k (Planar-Δz / dot(dir, camFwd))
                 (S, σt) = fogInject[cur][x,y,k]
                 Sint = (S − S·exp(−σt·dz)) / max(σt, 1e-5)
                 L += T · Sint;   T *= exp(−σt·dz)
                 fogIntegrated[x,y,k] = (L, T)          // Werte am FERNEN Rand der Scheibe
```

- **Compute** (Metal/Vulkan/D3D): ein Thread pro Spalte, Schleife über k. Nur Lesen per SRV/Texture, Schreiben
  per UAV-Store — **keine UAV-Loads** (RGBA16F-Typed-UAV-Load ist auf D3D11 FL11_0 optional).
- **OpenGL (Fragment, E10):** pro Scheibe k ein Vollbild-Dreieck in Layer k (`glFramebufferTextureLayer`), der
  Shader schleift über 0..k aus `fogInject` (O(N²/2) Fetches, Medium 2080 pro Spalte = ~30 M Fetches).
  Gewählt statt Ping-Pong über die Vorgängerscheibe, weil Lesen und Schreiben derselben 3D-Textur in verschiedenen
  Layern ein GL-Feedback-Loop ist. Inject auf GL ebenso: N Draws, einer pro Layer.

### 6.2 Rausch-Offset (Wind)

Das Backend akkumuliert pro Frame `noiseOffset += windVec · windSpeed · dt · k` (Windvektor wie die Wolkendrift aus
`windDirection`/`windSpeed`); akkumuliert statt `Zeit × Wind`, damit ein Windwechsel nicht springt. Zeit in
`noise.w` für langsame Evolution (zweite Oktave gegenläufig).

### 6.3 Apply (Vollbild, Blend `ONE, SRC_ALPHA`)

```
P = rekonstruiert (Backend-depthParams), d = |P − cam|, z = dot(P − cam, camFwd), dir = (P − cam)/d
(Lv, Tv) = fogIntegrated(uv, t(min(z, D)))            // Hardware-Trilinear
falls z > D:  dv = D · d / z
              Tf = exp(−fogOpticalSegment(cam.y, dir.y, dv, d, density, falloff))   // heutige Formel ab dv
              Lf = fogCol(dir) · (1 − Tf)
sonst:        Tf = 1, Lf = 0
out.rgb = Lv + Tv · Lf;   out.a = Tv · Tf               // Ergebnis: C·Tv·Tf + Lv + Tv·Lf
```

Himmelspixel (Tiefe = fern): `d` auf 1e5 m klemmen; da `C = fogCol` dort, ist der Fernfeld-Anteil eine Identität,
übrig bleibt das Nahvolumen (Schächte vor dem Himmel). Rain/Snow-Dunst (`precipExtinction = 0.004·rain +
0.008·snow`, Startwerte) geht nur ins Volumen, nicht ins Fernfeld.

### 6.4 Transluzente: `heApplyFogT` (Preamble)

`MaterialGraph.cpp:1944` emittiert für `blendMode == Translucent` `heApplyFogT(...)` statt `heApplyFog(...)`.
`heApplyFogT` = dieselbe Rechnung wie Apply, Volumen per `texelFetch`-Trilinear auf **Binding 34**
(`texture` auf set 0, `heFogVolume`), Ergebnis `C·T + L`. `heApplyFog` (opak) ist bei `fogVol.x > 0.5` ein No-op —
dasselbe Muster wie `viewMode.x`/`ssr.w`. Ebenso der Reflexions-Spiegel `:1857`. Eingebaute Backend-Shader
bekommen im Volumen-Modus `fogDensity = 0` von ihrer Fill-Site.

Bindestellen für Binding 34 (alle nötig, sonst lehnt D3D12 die PSO ab): Metal-Pin (nächster freier
Textur-Index), HLSL-Pin t34 (Sampler tot, teilt s0 mit dem lebenden `heLandscapeWeights` — erlaubt nach der Regel
in `:2685`), `D3D12MaterialRootSignature.h` (Null-SRV t34), `D3D11MaterialBindings.h`, Vulkan-Descriptor-Layout,
GL-Textureinheit. Der Register-Test in `test_material_graph.cpp` muss danach weiter grün sein.

## 7. Nahtlosigkeit Nah-/Fernfeld

- Das Fernfeld setzt bei `dv` mit der **Transmittanz der letzten Scheibe** (`Tv`) fort, nicht bei 1.
- Die Umgebungs-Quelle im Volumen ist `σt · fogCol(dir)` — exakt die Quelle, die die analytische Formel implizit
  integriert. Mit `noiseAmount = 0`, ohne Bank/Dunst, mit `kSunShare = 0`, Sonnen- und Lokalterm = 0 ist
  Modus 1 **mathematisch gleich** Modus 0 bis auf den Diskretisierungsfehler. Das ist der Paritätszeuge (§9).
- Sonnenterm und `kSunShare` laufen über `seamFade` aus; an der Naht ist die Quelle also wieder genau
  `σt · fogCol(dir)`. Im Fernfeld steckt das Sonnenleuchten bereits in `fogCol` (Himmel).

## 8. Performance-Budget

**Alle Zahlen sind Ziele, nicht gemessen.** Bezug: Baseline Metal/M5 (`docs/perf-audit/baseline-2026-09-27.md`,
Tabelle B): Scene 1,5–3,5 ms, Shadow ~1 ms. Externer Anker (aus Erinnerung, nicht nachgeprüft): Wronski 2014
(AC IV) und Hillaire 2015 (Frostbite) rechnen 160×90×64 in ungefähr 1 ms auf PS4/Xbox-One-Klasse.

| Backend | Pfad | Ziel Medium | Ziel Low | Default-Qualität |
|---|---|---|---|---|
| Metal (M5) | Compute | Inject ≤ 0,30 · Integrate ≤ 0,05 · Apply ≤ 0,15 → **≤ 0,5 ms** | ≤ 0,2 ms | Medium |
| D3D12 / D3D11 / Vulkan | Compute | wie Metal auf vergleichbarer HW (NN-WS03 misst) | — | Medium |
| OpenGL ≥ 4.3 (Linux) / 4.1 (macOS) | Fragment pro Layer | ≤ 1,0 ms | ≤ 0,4 ms | **Low** |
| Software | — | 0 (Fähigkeit aus) | — | — |

Zusatzkosten ohne Nebel: 0 — alle drei Pässe laufen nur bei `FogVolumeData::enabled`. Der Metal-Encoder-Split
kostet einen Store/Load von HDR-Farbe + Tiefe (2840×1528: ~50 MB Bandbreite), nur bei aktivem Nebel; Optimierung
später (lineare Tiefe in ein memoryless Attachment schreiben und per Framebuffer-Fetch lesen).

**Messrezept:** `scripts/he_perf_capture.py` (Detailed-Capture, `--no-counters` für FPS), Pass-Scopes
`FogInject`, `FogIntegrate`, `FogApply` neu im Engine-Profiler; Pass-p50 an/aus vergleichen, Stromsparmodus und
Fremdlast notieren (siehe `perf-measurement-on-m5`). Szene `landscape.hescene` + Foggy-Preset.

## 9. Fallback, Parität, Tests

- **Capability:** `IRenderer::Capabilities::supportsVolumetricFog` (neu, `IRenderer.h:223 ff.`). False →
  Extractor setzt `enabled = false`, Inspector graut Modus 1 aus. Software: immer false. Ohne shaderc (keine
  Material-Kreuzkompilierung) ebenfalls false, falls die Kernel über `MaterialShaderLibrary` laufen (§10).
- **Abnahme Modus 0:** pixelgleich zu `main` — md5-A/B mit `HE_SKY_TIME` + AA + RENDERPATH gepinnt, je Backend.
- **Paritätszeuge Modus 1:** Schalter `HE_FOG_PARITY=1` (noise/bank/dunst/Sonne/Lokal/kSunShare = 0) gegen Modus 0:
  Abweichung pro Kanal ≤ 2/255 außerhalb der ersten Scheibe — **gemessen nur auf Graph-Material-Pixeln** (z. B.
  Landschaft mit Auto-Landscape-Material), weil nur die schon heute `heSkyEnv` als Nebelfarbe nehmen. Auf eingebaut
  schattierten Pixeln (prozedurales `skyColor()` gegen Cube pro Spalte) ist es nur eine Sichtprüfung.
- **Unit-Tests** `tests/test_fog_volume.cpp`: Scheibenabbildung Hin-/Rückweg; `fogOpticalSegment(0,d)` = heutige
  Formel (Grenzfall k→0 inklusive); CPU-Integrator mit konstanter/Höhendichte gegen `exp(−optical)` (relativer
  Fehler < 1 % bei N = 64); `fogHistoryAlpha`; std140-Offsets von `FogVolumeConstants` (static_assert + Test).
- **Bildzeugen** (je Backend, HE_DUMP): (a) Foggy + tiefe Sonne + Säulen/Bäume → Schatten-Schächte sichtbar;
  (b) Bank 3 m über Wasser/Tal; (c) Punktlicht im Nebel; (d) Kamera unter Wasser → Volumen aus. Zeugen müssen ihr
  Motiv zeigen (Schacht im Bild), sonst beweisen sie nichts.

## 10. Shader-Quellen

Vier Sprachen, eine Mathematik. Entscheidung: **eine GLSL-450-Quelle** `FogVolumeShaderSource.h` mit Kernel-
Funktionen (`heFogInjectAt(ivec3)`, `heFogIntegrateAt(ivec2, int)`, `heFogApply(uv, depth)`) plus Einstiegs-Wrappern
(Compute für Vulkan; Fragment-pro-Layer für GL). MSL und HLSL per SPIRV-Cross zur Laufzeit wie die Material-
und Resolve-Shader (`MaterialShaderLibrary`) — dadurch stehen `heCsmShadow`/`heLocalShadowFactor`/`heCloudShadowFactor`
aus der Preamble ohne siebte Handkopie zur Verfügung. **Risiko:** der Pin-Mechanismus kennt heute nur Vertex/
Fragment; Compute-Stufe über SPIRV-Cross ist in Schritt 2 als Erstes zu prüfen. Fällt das, Rückfall auf
Handkopien (GLSL/HLSL/MSL wie die GI-Kernel) mit String-Vergleichstest der Konstanten nach dem Muster
„GI kernels: the constants the hand-kept copies must share“ in `test_culling.cpp`.

## 11. Wetter- und Szenen-Hooks

- `WeatherPreset` bekommt `fogBank` (Bankdichte) und `fogNoise`; Foggy z. B. `fogBank 0.15, fogNoise 0.7`, Rain
  `fogBank 0.02`. `drive()` + `last*`-Sentinel pro neuem Feld wie `lastFog` (`WeatherSystem.cpp:160-175`); der Nutzer
  kann jeden Wert übernehmen, bis der nächste Preset ihn zurückholt.
- Niederschlags-Dunst aus `rainAmount`/`snowAmount` (keine neuen Felder), Blitz aus `flash`, Wind aus
  `windDirection`/`windSpeed` — alles schon in `EnvironmentSettings`.
- Tag/Nacht: über das Richtungslicht (Sonne/Mond) automatisch.
- Unterwasser: `UnderwaterCameraData::wet` → `enabled = false` (der Unterwasser-Pass übernimmt). Im
  Wasserlinienband (dry, density > 0) laufen beide.
- **Nicht in v1** (Phase 2): `FogVolumeComponent` (lokale Nebeltaschen, Box/Kugel, bis 16 Stück, Inject nimmt ein
  Array wie `waterRippleA[16]`; Registrierung siehe Gedächtnis „Neue Komponente: sechs Stellen“), Licht-Faktor
  `volumetricScatter` pro `LightComponent`, Cluster-Lichter im Inject, Nebel auf Partikeln (heute ebenfalls
  unvernebelt, `ParticleShaderTemplates.h` hat keinen Fog), eingebaute Transparenz-Draws (falls es sie gibt,
  bekommen sie in v1 keine Volumen-Lesung — in Schritt 2 prüfen).

## 12. Schrittplan (Vorschlag für die Queen)

1. ~~Entwurf~~ (dieses Dokument).
2. **Host-Seite:** `FogVolume.h` + `test_fog_volume.cpp`; Env-Felder (X-Liste, HcNodeDocs, Push, Inspector, Hilfe,
   Serializer-Tests inkl. alter Szene); `FogVolumeData` + Extractor; `supportsVolumetricFog`. Kein Bildunterschied.
   Dazu der Machbarkeitscheck Compute über SPIRV-Cross (§10).
3. **Metal:** Inject/Integrate (Compute), Apply + `hdrPass`-Split, `heApplyFogT` + Binding 34, Zeugen, Messung.
4. **OpenGL** (Fragment-Pfad), Zeugen per Headless-Dump auf dem Mac.
5. **Vulkan** (MoltenVK-Syntaxcheck lokal, lavapipe-CI-Bildtest).
6. **D3D11 + D3D12** — Hardware-Abnahme auf NN-WS03 (bleibt offen, bis der Rechner da ist).
7. Wetter-Hooks (§11), Default für neue Szenen (`fogMode = 1`?), In-Engine-Handbuch, Perf-Abnahme aller Backends.
8. Phase 2: lokale Nebelvolumen, Partikel, Cluster-Lichter.

## Handoff

- **Topic / step:** 225 (Echtes volumetrisches Nebel), Schritt 1 von 1 (Plan im Board); Vorschlag Schritte 2–8 in §12
- **Branch / commit:** `claude/echtes-volumetrisches-nebel-statt-hintergrund-blend`, Commit siehe Hive-Post (pushed: siehe dort)
- **Done:** Entwurf mit Ansatz, Datenformaten, Schnittstellen, Einbaustellen pro Backend, Budget, Fallback, Tests.
- **Files touched:** `docs/volumetric-fog-plan.md` (neu). Kein Code.
- **Verified:** Fog-Lesestellen, Env-Felder/X-Listen, Weather-Presets, Sampler-Budget (`MaterialShaderLibrary.cpp:2655 ff.`),
  freie Binding-Nummer 34, `Lighting` append-only-Versionierung, Capabilities-Struct — per `grep -n`/Lesen.
  Pass-Reihenfolge, Shadow-Formate, Unterwasser-Pass-Verdrahtung, Compute/3D-Texturen pro Backend: zwei Survey-Agenten.
- **Not verified:** alle ms-Zahlen (Ziele); externe Kostenanker (aus Erinnerung); ob SPIRV-Cross-Compute über den
  Pin-Mechanismus geht; GL-4.1-Textureinheiten-Budget für transluzente Graph-Materialien mit Binding 34; ob eingebaute
  Shader transluzent zeichnen; ob `heSkyEnv` Mips hat (für eine weichere Umgebungsfarbe).
- **Decisions taken:** E1–E10 (§0).
- **Decisions suggested, not taken:** Default `fogMode` für neue Szenen; Werte der neuen Preset-Felder; ob GL ≥ 4.3
  später den Compute-Pfad bekommt.
- **Traps:** Tiefenrekonstruktion nur über `depthParams` des Backends (Thema 185); D3D-Sampler 16/16 — kein neuer lebender
  Sampler in der Preamble; RGBA16F-UAV-Loads vermeiden (D3D11); GL: Tiefe nie gleichzeitig gebunden und attached;
  `test_hc_node_docs` merkt fehlende Docs nur im vollen ctest.
- **Next steps:** 1. `HorizonRendering/FogVolume.h` + `tests/test_fog_volume.cpp` (Tests §9 zuerst). 2. Env-Felder §2.1 mit
  allen Mitzieh-Stellen. 3. `FogVolumeData` im Extractor, `supportsVolumetricFog` (alle false). 4. Wegwerf-Prototyp:
  GLSL-450-Compute durch den SPIRV-Cross-Pfad nach MSL — Ergebnis bestimmt §10.
