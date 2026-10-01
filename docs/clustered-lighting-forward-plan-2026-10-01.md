# Clustered Lighting für Graph-Materialien (Forward) — IST, Shader-Seite, Backend-Plan

> Stand: 2026-10-01 · Thema 117, Schritt 1 · Zweig `claude/clustered-lighting-forward-parity`.
> Grundlage: `docs/deferred-d3d-vulkan-analysis-2026-10-01.md` (Thema 116, §3.1/§7).

## 1. IST-Stand (vor diesem Schritt, Code auf main `0513f8d4`)

| | eingebauter Szenen-Shader (Forward) | Graph-Materialien (`heLitP`) | Deferred-Resolve |
|---|---|---|---|
| **D3D11** | **echt geclustert**: `StructuredBuffer` t18–t20, `HE::BuildClusterLights`, 256 Lichter (`D3D11Renderer.cpp:469`, `:3944`, `:5622`) | 8er-Fenster | — |
| **D3D12** | **echt geclustert**: Root-SRVs t18–t20 (`D3D12Renderer.cpp:503`, `:4760`, `:9188`) | 8er-Fenster | — |
| **Vulkan** | **echt geclustert**: SSBO-Bindings 10–12 (`shaders/scene.frag:121`, `VulkanRenderer.cpp:1766`, `:6267`) | 8er-Fenster | — |
| **OpenGL** | 8er-Fenster (`kUnlitFS`, GLSL 4.10) | 8er-Fenster | 8er-Fenster |
| **Metal** | 8er-Fenster (`fragmentMain`, keine Cluster-Puffer) | 8er-Fenster | **geclustert** (`deferredResolveClustered`, eigene Scatter-Kopie `EncodeClusterData`) |

Die offene Frage aus Thema 116 („clustert der Forward-Zwilling nur die ersten 8?") ist
damit beantwortet: **nein**, D3D11/D3D12/Vulkan clustern im eingebauten Shader echt über
Structured Buffers/SSBOs. Die Lücke ist ausschließlich `heLitP`: die gemeinsame Preamble
hatte keinen Cluster-Code, also sah jedes Graph-Material auf **allen fünf** Backends nur
das 8-Licht-Fenster. Der einzige Cluster-Code in `MaterialShaderLibrary` steckte im
Deferred-Resolve und war per `compileResolveVariant` auf Metal gesperrt.

## 2. Was dieser Schritt geändert hat (nur gemeinsame Shader-/Datenseite)

- **`MaterialShaderLibrary::fragmentClustered(hash, glsl, backend)`** — Zwilling von
  `fragment()`. Definiert `HE_CLUSTERED` vor der Preamble; dann deklariert die Preamble
  die drei Listen (Bindings **24/25/26**, dieselben wie der Resolve) und `heLitP` shadet
  Punkt-/Spotlichter aus der Cluster-Liste des Fragments (`heFwdClusterLights`,
  `heFwdClusterShadow`). Eigener Cache-Slot. GLSL410/ES300 → `ok = false` mit Log.
- **Lighting-ABI, append-only v3.3:** `clusterParams` (x/y/z Grid, w Slice-Scale,
  **x == 0 → aus**) und `clusterCamFwd` (xyz Blickrichtung, w Near). Unbedingt im
  `HeLighting`-Block, weil `wpoLightingBlock()` ihn für den WPO-Vertex ausschneidet
  (GL-Link-Regel). `sizeof(Lighting)` wächst um 32 B auf 2080 B; alle Backends binden per
  `sizeof` (Metal `setFragmentBytes` < 4 KiB, D3D12 `alignUp`).
- **`HE::FillMaterialClusterParams(build, lighting)`** — kopiert `params`/`camFwd` aus
  dem `ClusterLightBuild`, sonst nichts.
- **`HE::BuildClusterLights(..., bottomLeftOrigin)`** — GL-Fill-Sites übergeben `true`
  (gl_FragCoord unten-links), dann entfällt der v-Flip und der Shader bleibt einheitlich.
- **Pins:** Metal → Fragment-Buffer **4/5/6** (`kMetalCluster*BufferIndex`, im
  Szenen-Pass frei, dieselben wie der Resolve). HLSL → **t24/t25/t26**, kein Sampler.
- **Tests:** `test_material_graph.cpp` (Variante pro Backend, Register/Typ, Sampler-Liste
  unverändert, GL lehnt ab, WPO-Block; Windows: FXC-Sweep aller Knoten als Cluster-Variante
  + geloggte Gradient-Probe), `test_culling.cpp` (Ursprungs-Spiegelung, Fill-Helfer,
  Drift-Guard Forward-Zwilling ↔ Resolve).

### 2.1 Vertrag, auf den die Backend-Schritte bauen

1. **Das 8er-Fenster bleibt voll** (`FillMaterialLightWindow` wie bisher). Die
   Cluster-Variante überspringt Fenster-Punkt/Spot selbst, solange das Gate an ist.
   Grund: D3D11/D3D12/Vulkan nehmen vorkompilierte Pak-Blobs (`precompiledFor`, alte
   Preamble ohne Cluster-Code) und teilen sich pro Frame **einen** Lighting-Puffer mit
   allen Material-Draws. Ein direktional-only gefülltes Fenster ließe diese Materialien
   ohne jedes Punktlicht. **Nicht** auf `BuildDirectionalLightWindow` umstellen.
2. **Gate nur setzen, wo alle drei Puffer gebunden sind.** Wer die Variante benutzt,
   muss t24–t26 / Buffer 4–6 / Bindings 24–26 binden (auch bei Gate 0: D3D12-Root-Signatur
   und Vulkan-Set-Layout verlangen sie statisch; auch unbeleuchtete Domänen deklarieren
   sie, weil glslang Deklarationen nicht entfernt).
3. **`giParams.xy` (Viewport) füllen, sobald `clusterParams.x > 0`** — heute ist es nur
   für GI/AO/SSR gefüllt; die Zellwahl teilt `gl_FragCoord` dadurch.
4. Cluster-Lichter tragen Atlas-Layer (`params.y`) nur mit `localShadowsActive`, GI-Kanal
   (`params.z`) nur mit `giMasksValid` — dieselben Flags wie beim eingebauten Shader.
5. Im Cluster-Loop **`textureLod`, nicht `texture`**: die Trip-Count variiert pro Pixel,
   FXC lehnt Gradient-Samples darin ab (X3570). Atlas und GI-Maske haben eine Mip.

## 3. Plan: fehlende Backend-Bindungen (je ein Schritt)

| Backend | Bindung | Was fehlt | Zeuge |
|---|---|---|---|
| **D3D11** | t24–t26 | Drei Puffer mit `D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS` + SRV `DXGI_FORMAT_R32_TYPELESS`, `D3D11_BUFFEREX_SRV_FLAG_RAW` (SPIRV-Cross emittiert `ByteAddressBuffer`; die strukturierten t18–t20 sind dafür **nicht** verwendbar — ein Puffer kann nicht STRUCTURED und RAW zugleich). Alternativ den Hand-HLSL auf `ByteAddressBuffer` umstellen und einen Puffersatz teilen. Materialpfad (`:4306`) auf `fragmentClustered`, Fill: `FillMaterialClusterParams` aus dem schon vorhandenen Build (`:5622`). Exporter-/Pak-Variante siehe §4. | FXC-Sweep (CI Windows), WARP-Pixeltest mit > 8 Lichtern |
| **D3D12** | t24–t26 | `D3D12MaterialRootSignature.h`: drei Root-SRVs (oder eine Range) für t24–t26; Root-SRVs sind raw/structured-agnostisch, der vorhandene Ring (`:4760`) kann direkt dran. PSO-Warmup prüfen. | WARP-PSO-Test (`test_material_graph.cpp`, Thema 56) um t24–t26 erweitern, dann Pixeltest |
| **Vulkan** | Set 0, 24–26 | `m_matSetLayout` (`:2354`) um drei `STORAGE_BUFFER` (Fragment) erweitern, Pool-Größen, Material-Descriptor-Writes auf die vorhandenen Cluster-SSBOs (`:1863`). | nur Kompilat (kein Laufzeit-Zeuge in CI; MoltenVK lokal nur Syntax) |
| **Metal** (Forward) | Buffer 4/5/6 | **Erledigt in Schritt 2**, siehe §3.1. | he_shot A/B (md5) lokal, `MANYLIGHTS=16` |
| **OpenGL** (≥ 4.3) | SSBO 24–26 | Neues Target `Glsl430` in `ShaderCompiler.h` (+ `Backend::GLSL430`), Laufzeitwahl nach `GLAD_GL_VERSION_4_3` (wie `m_giSupported`, `:3082`); SPIRV-Cross-GLSL braucht die Binding-Nummern explizit (`layout(binding=…)`, GL 4.3 hat sie). Fill mit `BuildClusterLights(..., bottomLeftOrigin = true)`. macOS-GL (4.1) bleibt beim Fenster. | keiner auf diesem Mac (GL 4.1); nur CI-Kompilat |

### 3.1 Metal-Forward (Schritt 2, erledigt)

- `EncodeClusterData` hat keine eigene Scatter-Kopie mehr: `BuildFrameClusterLights()`
  ruft `HE::BuildClusterLights` (Atlas-Lane an `m_localShadowTex`, GI-Kanal am selben
  Gate wie vorher), `BindClusterBuffers()` lädt die drei Listen in den Frame-Ring und
  bindet sie auf `kMetalCluster*BufferIndex` (4/5/6). Die doppelten `kCluster*`-Konstanten
  im Header sind weg.
- `EncodeScene` baut **einen** Build pro Frame direkt nach `FillMaterialLighting`, bindet
  4/5/6 einmal für den ganzen Pass (Sky/Skinned fassen nur 0–3 an) und setzt das Gate per
  `HE::FillMaterialClusterParams` in `matLight`. Das Fenster bleibt voll (§2.1/1). Der
  Stored-Resolve nimmt denselben Build, der Tile-Resolve (eigener Encoder) baut seinen.
- `GetOrBuildMaterialPipeline`: Forward-PSOs (opak + blended) aus `fragmentClustered`,
  bei Compile-Fehler Rückfall auf `fragment()` mit Warnung; G-Buffer-Variante und
  vorkompilierte Blobs unverändert. Die Materialvorschau bindet leere Listen (Gate 0).
- `HE_FORWARD_CLUSTER=0` (einmal in `Initialize` gelesen, wie auf D3D/Vulkan) = alter Pfad.
- **Zeuge** `HE_DUMP_MANYLIGHTS=N[builtin]` (EditorApplication): Graph-Material-Boden unter
  zwei Reihen aus N bunten Punktlichtern bei y≈200. Aufnahme:
  `HE_SKY_TIME=10 scripts/he_shot.py out.png MANYLIGHTS=16 TOD=0 COVERAGE=0 CLOUDMODE=0 AA=0 CAMY=207 CAMZ=2 PITCH=-38 RENDERPATH=0`.
  Ergebnis (md5, Release, M5): vorher 8 von 16 Lichtpools, nachher 16/16; die 8 vorher
  schon beleuchteten Pools sind bis auf 2 Pixel ±1 identisch. `HE_FORWARD_CLUSTER=0`,
  Deferred (`RENDERPATH=1`), die Built-in-Kontrolle (`MANYLIGHTS=16builtin`) und
  `MATERIALTEST=switchon` sind bitgleich zu vorher. Unter `MTL_DEBUG_LAYER=1` meldet
  der Lauf keine fehlende Bindung auf 4/5/6 (siehe §5 zu den Samplern).

## 4. Pak-Varianten (gilt für D3D11/D3D12/Vulkan)

Der Export backt heute `fragment()`. Sobald ein Backend die Cluster-Variante bindet,
sollte der Exporter (`ExportDialogPanel.cpp:353`, `EditorApplication.cpp:5028`) für dieses
Backend `fragmentClustered()` backen — sonst bleiben ausgelieferte Spiele beim 8er-Fenster
(funktional korrekt dank §2.1/1, aber ohne Gewinn). Ein alter Blob am neuen Backend ist
harmlos: Fenster voll, Gate wird nicht gelesen. Das `MaterialShaderVariant`-Format muss dafür
kenntlich machen, welche Variante gebacken ist (sonst bindet ein Backend die Puffer für
einen Blob, der sie nicht deklariert — auf D3D11/Vulkan unschädlich, auf D3D12 nur, wenn die
Root-Signatur sie optional abdeckt).

## 5. Nebenbefunde (nicht in diesem Schritt gelöst)

- Der **eingebaute** Forward-Shader von **Metal** (`fragmentMain`) und **GL** (`kUnlitFS`)
  bleibt beim 8er-Fenster. Das ist eine zweite Licht-Limit-Lücke neben `heLitP`.
- Der Deferred-Resolve-Kommentar im Header nennt die GI-Lokalmaske „v1 limitation", der
  Code wendet sie längst an (Kanal über `params.z`).
- Der Resolve sampelt im Cluster-Loop mit `texture()`. Für einen späteren HLSL-Resolve
  (Thema 116, §4.1) muss er auf `textureLod` wie der Forward-Zwilling — der Drift-Guard
  normalisiert genau diesen Unterschied.
- (Schritt 2) Unter `MTL_DEBUG_LAYER=1` scheitert ein Material-Draw schon **vor** dieser
  Änderung an fehlenden **Samplern** 5–12/14 (`heGIShadowSmplr` … `heSkyEnvSmplr`),
  identisch mit `HE_FORWARD_CLUSTER=0` und `=1` (Szene `MANYLIGHTS=16` +
  `MATERIALTEST=translucent` + `PREVIEW=1`). Im Normalbetrieb harmlos, aber es versperrt
  Validierungsläufe.
- (Schritt 2) Im Zeugen ist der Boden im Deferred-Pfad deutlich heller als im Forward-Pfad
  (Ambient/IBL), bei gleicher Szene. Nicht untersucht.
- Der Exporter backt für Metal weiterhin `fragment()` (§4) — ausgelieferte Spiele bleiben
  bis dahin beim 8er-Fenster.
