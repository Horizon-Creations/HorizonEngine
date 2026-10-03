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
| **D3D11** | t24–t26 | **Erledigt in Schritt 4**, siehe §3.3. | WARP-Pixeltest mit 12 Lichtern (CI Windows); HW-Pixeltest offen |
| **D3D12** | t24–t26 | **Erledigt in Schritt 3**, siehe §3.2. | WARP-PSO-Test + Abdeckungs-Sweep (CI Windows); Pixeltest offen |
| **Vulkan** | Set 0, 24–26 | **Erledigt in Schritt 5**, siehe §3.4. | SPIR-V-Reflexion aller Knoten gegen die Layout-Tabelle (CI alle Plattformen) + MSVC-Kompilat (CI Windows); Pixeltest offen |
| **Metal** (Forward) | Buffer 4/5/6 | **Erledigt in Schritt 2**, siehe §3.1. | he_shot A/B (md5) lokal, `MANYLIGHTS=16` |
| **OpenGL** (≥ 4.3) | SSBO 4–6 | **Erledigt in Schritt 6**, siehe §3.5. macOS-GL (4.1) bleibt beim Fenster. | glslang-GL-Link aller Knoten-Shader (CI alle Plattformen); Laufzeit auf GL 4.3 offen |

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

### 3.2 D3D12 (Schritt 3, erledigt bis auf Pixel-Zeugen)

- `D3D12MaterialRootSignature.h`: drei **Root-SRVs** t24/t25/t26 als Parameter **6/7/8**
  (`kRootCluster*`, Pixel-Sichtbarkeit), `kParamCount` 6 → 9 (17 DWORDs). Root-Deskriptoren
  haben keine View, also ist raw (`ByteAddressBuffer` aus SPIRV-Cross) vs. structured egal.
  `kPreClusterParamCount = 6` + `paramCount`-Argument für die Negativkontrolle.
- Renderer: dieselben Indizes wie die Szenen-/Skinned-Signatur (`static_assert`), daher hängt
  `bindClusterRoots()` direkt nach `SetGraphicsRootSignature(m_matRootSig)` die **vorhandenen**
  Upload-Ringe (t18–t20 des eingebauten Shaders, gleiches Byte-Layout) auch an t24–t26.
  Kein zweiter Puffersatz, kein zweiter Build.
- `GetOrBuildMaterialPSO`: Cross-Compile nimmt `fragmentClustered`, solange
  `matClustered()` (Ringe da, `HE_FORWARD_CLUSTER` ≠ 0); Cross-Compile- oder FXC-Fehler →
  Warnung + Rückfall auf `fragment()`. Gebackene Pak-Blobs unverändert (§4). Die Wahl ist
  vor jedem Material-PSO entschieden: `m_matReady` wird erst nach Ring-Aufbau und
  Env-Lesen gesetzt.
- `DrawScene`: `uploadClusters` legt den Build in `frameClusters` ab, `fillMatLight` setzt
  das Gate per `FillMaterialClusterParams` (nur wenn `clustered`, also Ringe gebunden und
  Lichter da). Fenster bleibt voll (§2.1/1); `giParams.xy` war schon Viewport (§2.1/3).
- **Zeuge (CI Windows):** `test_material_graph.cpp` — WARP-Case: Cluster-PS bindet
  `D3D_SIT_BYTEADDRESS` auf t24–t26, der plain PS nicht; PSO gegen die volle Signatur baut,
  gegen die Vor-Cluster-Signatur `E_INVALIDARG`, plain PS dort weiterhin OK. Sweep: alle
  Knoten-Shader auch als Cluster-Variante reflektiert und gegen die Signatur gedeckt
  (Root-SRV-Zweig in `coveredBy`), Vor-Cluster-Signatur als Negativkontrolle.
- **Offen:** Pixel-Zeuge auf echter HW (NN-WS03), CI hat keinen D3D12-Bildlauf. Rezept wie
  Metal §3.1, nur mit `HE_DUMP_RHI=D3D12`: `HE_DUMP_MANYLIGHTS=16` einmal mit, einmal mit
  `HE_FORWARD_CLUSTER=0`; erwartet 16 statt 8 Lichtpools, `16builtin` unverändert.

### 3.3 D3D11 (Schritt 4, erledigt bis auf Pixel-Zeugen auf HW)

- **Zweiter, roher Puffersatz.** Der eingebaute Shader liest die Listen als
  `StructuredBuffer` auf t18–t20, SPIRV-Cross macht aus den SSBOs der Cluster-Variante
  `ByteAddressBuffer` auf t24–t26. Ein D3D11-Puffer kann nicht `MISC_BUFFER_STRUCTURED`
  und `MISC_BUFFER_ALLOW_RAW_VIEWS` zugleich tragen, also gibt es drei rohe Zwillinge
  (`clusterLightRaw`/`GridRaw`/`IdxRaw`, gleiche Bytegrößen), gefüllt aus **demselben**
  `ClusterLightBuild`. Den Hand-HLSL auf `ByteAddressBuffer` umzustellen hätte einen
  Satz gespart, aber den eingebauten Pfad angefasst; das ist bewusst nicht passiert.
- `D3D11MaterialBindings.h`: `kCluster*SrvSlot` (24/25/26), `CreateClusterRawBuffer`
  (DYNAMIC, `ALLOW_RAW_VIEWS`, SRV `R32_TYPELESS` + `BUFFEREX_SRV_FLAG_RAW`) und
  `BindClusterLists`. Renderer und WARP-Test benutzen dieselben Helfer.
- Renderer: Zwillinge nur, wenn der strukturierte Satz steht (alle drei oder keiner,
  sonst Warnung). `matClustered()` = `HE_FORWARD_CLUSTER` ≠ 0 und Zwillinge da, beides
  vor `createMaterialResources()` entschieden. `uploadClusters` legt den Build in
  `frameClusters` ab, lädt ihn zusätzlich in die Zwillinge und bindet t24–t26 (einmal pro
  Fill, kein anderer Pass fasst die Register an). `fillMatLight` setzt das Gate per
  `FillMaterialClusterParams` nur in diesem Fall. Fenster bleibt voll (§2.1/1),
  `giParams.xy` war schon der Viewport (§2.1/3).
- `GetOrBuildMaterialShaders`: Cross-Compile nimmt `fragmentClustered`, Fehler beim
  Cross-Compile oder in FXC → Warnung + Rückfall auf `fragment()`. Gebackene Pak-Blobs
  unverändert (§4).
- **Zeuge (CI Windows, WARP):** `test_material_graph.cpp`, „D3D11: a clustered graph
  material is lit by cluster lights beyond the 8-light window". 1×1-Draw, 1×1×1-Gitter,
  12 Listenlichter über `CreateClusterRawBuffer`. Die ersten 8 sind schwarz (und stehen im
  Fenster), 9–12 rot mit je 0,5. Positivkontrolle: Fenster-PS mit einem roten Licht der
  Stärke 2. Erwartet: Cluster-PS mit Gate = Kontrollpixel; Fenster-PS auf demselben CB
  schwarz; Cluster-PS mit Gate 0 schwarz, mit rotem Fensterlicht = Kontrollpixel.
  Reflexion: `D3D_SIT_BYTEADDRESS` auf t24–t26 im Cluster-PS, nicht im plain PS.
- **Offen:** Pixel-Zeuge auf echter HW (NN-WS03), Rezept wie §3.2 mit
  `HE_DUMP_RHI=D3D11`: `HE_DUMP_MANYLIGHTS=16` einmal mit, einmal mit
  `HE_FORWARD_CLUSTER=0`; erwartet 16 statt 8 Lichtpools, `16builtin` unverändert.

### 3.4 Vulkan (Schritt 5, erledigt bis auf Pixel-Zeugen)

- **Kein zweiter Puffersatz.** Die drei Cluster-SSBOs pro Frame-Slot
  (`m_clusterLights/Grid/Idx`, Szenen-Set-Bindings 10–12 des eingebauten Shaders) haben
  dasselbe Byte-Layout wie die std430-Listen der Cluster-Variante. Jedes Material-Set
  bekommt sie zusätzlich auf Bindings **24/25/26** geschrieben. Ein Build pro Frame
  (`frameClusters` in `DrawScene`, aus dem Frame-UBO-Block hochgezogen) bedient beide.
- **`VulkanMaterialLayout.h`** (`HE::vkmat::kBindings`): die eine Tabelle, aus der
  `createMaterialResources` das Set-Layout baut **und** die Pool-Größen zählt
  (`countOf(kind)`). Ohne Storage-Eintrag im Pool scheitert `vkAllocateDescriptorSets`,
  und die Draw-Schleife überspringt jedes Graph-Material stumm; deshalb zählt der Pool
  aus derselben Tabelle wie das Layout. Kein `vulkan/vulkan.h` im Header (he_tests baut
  auf macOS/Linux ohne SDK), Stage-Bits per `static_assert` gegen Vulkans Werte. Die drei
  Cluster-Zeilen stehen zuletzt, `kPreClusterBindingCount` schneidet sie für die
  Negativkontrolle ab.
- Per-Draw-Writes auf 24–26, sobald `m_clusterReady` (auch bei Gate 0: eine geclusterte
  Pipeline nutzt sie statisch, ihr Set muss vollständig sein). Ohne SSBOs ist jede Pipeline
  die plain-Variante, die keines der drei Bindings nutzt.
- `GetOrBuildMaterialPipeline`: Cross-Compile nimmt `fragmentClustered`, solange
  `m_forwardClustered && m_clusterReady` (beides in `createScenePipeline` entschieden,
  vor `createMaterialResources`); Fehler → Warnung + Rückfall auf `fragment()`. Gebackene
  Pak-Blobs unverändert (§4). `HE_FORWARD_CLUSTER=0` = plain-Pipelines, Gate 0.
- Lit-Fill: `FillMaterialClusterParams(frameClusters, lit)` nur wenn `clustered`
  (SSBOs da, Guard nicht gesetzt, Lichter vorhanden). Fenster bleibt voll (§2.1/1),
  `giParams.xy` war schon der Viewport (§2.1/3).
- **Zeuge (CI, alle Plattformen):** `test_material_graph.cpp`, „Vulkan: the clustered
  variant adds exactly set 0 SSBOs 24..26, all in the material layout". SPIRV-Cross
  reflektiert die **statisch benutzten** Deskriptoren (`get_active_interface_variables`)
  beider Varianten jedes Knoten-Shaders: die Cluster-Variante fügt genau die drei
  Storage-Buffer in Set 0 hinzu (oder nichts, wo heLitP nicht erreicht wird), nimmt
  nichts weg, die Tabelle deckt sie, die Vor-Cluster-Tabelle nicht; der plain-Shader fasst
  24–26 nie an. Negativkontrolle der Reflexion: ein Probe-Shader mit gelesenem SSBO auf
  40 und deklariertem, ungelesenem Sampler auf 41 meldet genau das SSBO. Dazu das
  MSVC-Kompilat von `VulkanRenderer.cpp` im Windows-Job (lokal vorab: clang
  `-fsyntax-only` gegen MoltenVK, mit Negativkontrollen).
- **Offen:** Pixel-Zeuge auf echter HW (NN-WS03), CI hat kein Vulkan-ICD. Rezept wie §3.2
  mit `HE_DUMP_RHI=Vulkan`: `HE_DUMP_MANYLIGHTS=16` einmal mit, einmal mit
  `HE_FORWARD_CLUSTER=0`; erwartet 16 statt 8 Lichtpools, `16builtin` unverändert.
  Mit Validierungs-Layer wäre zusätzlich zu prüfen, dass keine Meldung zu Bindings 24–26
  kommt.

### 3.5 OpenGL ≥ 4.3 (Schritt 6, erledigt bis auf Pixel-Zeugen)

- **Neues Target `Glsl430`** (`ShaderCompiler.h`) + `compileGlslPinned` (`GlslPin`), im
  Stub mitdefiniert (sonst linken die App-Flavours nicht). Ab GLSL 4.20 schreibt
  SPIRV-Cross `layout(binding = N)` auf jede Ressource mit Binding-Dekoration. Der
  GL-Renderer bindet Uniform-Blöcke und Sampler aber **per Name** (4.10-Modell), also
  entfernt `Glsl430` deren Bindings; nur Storage-Blöcke behalten eins (per Name erst nach
  erfolgreichem Compile umhängbar). `Backend::GLSL430` hinten an das Enum angehängt
  (Cache-Salze bleiben kollisionsfrei).
- **SSBO-Bindings 4/5/6, nicht 24–26** (`kGlCluster*SsboBinding`): GL 4.3 garantiert nur
  8 SSBO-Bindings, ein Binding ≥ dem Treiberlimit ist ein **Compile**-Fehler. 0–3 gehören
  den GI-Compute-Pässen. Symmetrisch zu Metals Buffer 4/5/6.
- **Programm:** beide Stufen GLSL 4.30 (`standardVertex`/`customVertex(GLSL430)` +
  `fragmentClustered(GLSL430)`), eine Version pro Programm. Rückfall auf das 4.10-Paar
  bei Cross-Compile-Fehler **und** bei Link-Fehler (Warnung), erst danach wird 0 gecacht.
  G-Buffer-Variante (`gbuffer=true` an beiden Aufrufstellen) und gebackene Pak-Varianten
  (GLSL 4.10, plain) unverändert.
- **Entscheidung in `Initialize`:** `m_forwardClustered` = `GLAD_GL_VERSION_4_3` und
  `HE_FORWARD_CLUSTER` ≠ 0, eigenes Flag (nicht `m_giSupported`, das bei GI-Fehlern
  zurückfällt), vor `WarmupMaterials`. `CreateClusterSSBOs` legt die drei Puffer mit je
  einem Null-Eintrag an und bindet sie sofort, damit Vorschau/Thumbnail (gleiches
  Programm, Gate 0) nie ein ungebundenes SSBO lesen. macOS (GL 4.1) bleibt beim Fenster.
- **DrawScene:** ein `BuildClusterLights(..., localShadows, giShadingActive &&
  m_giLocalMaskTex, bottomLeftOrigin = true)` pro Frame, vor dem ersten `fillMatLight`;
  `UploadClusterLists` lädt und bindet 4/5/6 neu. Gate per `FillMaterialClusterParams` in
  `fillMatLight`. Fenster bleibt voll (§2.1/1), `giParams.xy` war schon der Viewport
  (§2.1/3). `bottomLeftOrigin` geprüft: die Szene rendert ohne Y-Flip mit
  `glViewport(0, 0, pw, ph)` in `m_hdrFBO` bzw. den G-Buffer. Der GL-Deferred-Resolve
  (GLSL 4.10, eigener `HeResolve`-Block) liest das Gate nicht.
- **Zeuge (CI alle Plattformen + lokal):** `test_material_graph.cpp`, „OpenGL 4.3: the
  clustered variant is GLSL 4.30 with SSBOs 4..6 only and links with its vertex". Für
  alle 75 Knoten-Shader: `#version 430`, Storage-Blöcke genau auf 4/5/6 (75/75), kein
  `binding` auf Uniform-Block/Sampler, Vertex ohne Binding, und **glslang im reinen
  OpenGL-Modus** parst beide Stufen und linkt sie (`TProgram::link` prüft gleichnamige
  Blöcke stufenübergreifend). Negativkontrollen: ein Block-Mismatch zwischen den Stufen
  wird abgelehnt; ein Probe-Shader zeigt, dass `Glsl430` UBO-/Sampler-Bindings entfernt,
  SSBOs behält und Pins nur in der passenden Stufe greifen.
- **Offen:** Laufzeit auf echtem GL 4.3 (NN-WS03 o. ä.; hier gibt es keins). Rezept wie
  §3.2 mit `HE_DUMP_RHI=OpenGL`: `HE_DUMP_MANYLIGHTS=16` einmal mit, einmal mit
  `HE_FORWARD_CLUSTER=0`; erwartet 16 statt 8 Lichtpools, seit §3.6 auch mit
  `16builtin` (vorher blieb `kUnlitFS` beim Fenster). Im Log muss „built a
  CLUSTERED material program (GLSL 4.30)" stehen, keine Rückfall-Warnung; mit
  `HE_GL_DEBUG=1` keine Meldung zu SSBO 4–6.

### 3.6 Eingebaute Forward-Shader Metal + GL (Thema 123, Nachzug Schritt 1)

Der Nebenbefund aus §5 („eingebauter Shader bleibt beim Fenster") ist für Metal und
GL ≥ 4.3 geschlossen. Gleicher Vertrag wie §2.1: Fenster bleibt voll, die Shader
überspringen dessen Punkt-/Spot-Plätze selbst, solange das Gate an ist.

- **Metal `fragmentMain`:** liest die Listen auf Fragment-Buffer **4/5/6** (dieselben
  wie heLitP). Gate + Raster in `SceneUniforms::clusterParams/clusterCamFwd` (angehängt,
  `static_assert` +32 B). `EncodeScene` baut den Cluster-Build jetzt **vor** dem Upload
  von `scene` (der Skinned-Pass lädt dieselbe Struct erneut hoch); ein Build dient
  fragmentMain, den Graph-Materialien und dem Stored-Resolve. Ohne Build
  (`HE_FORWARD_CLUSTER=0`) werden 16-Byte-Nulllisten gebunden, weil fragmentMain die
  Puffer unbedingt deklariert. `localShadowFactor` → `localShadowFactorAt(posType, base)`,
  der Fenster-Pfad ruft es unverändert auf. Kein Rückfall nötig: der Shader-Text ist
  für beide Fälle derselbe.
- **GL `kUnlitFS`:** `#ifdef HE_CLUSTERED`-Zweig (SSBOs 4/5/6 aus
  `kGlCluster*SsboBinding`, `uClusterParams/uClusterCamFwd`, `clusterLights()`).
  `SceneStageSource` hebt **beide** Stufen eines Szenen-Programms auf GLSL 4.30 und
  definiert Schalter + Bindings; `LinkSceneProgram` baut unlit/skinned/instanced damit,
  solange `m_forwardClustered`, sonst und bei Compile-/Link-Fehler (Warnung) die
  4.10-Fassung. `m_forwardClustered` bleibt dabei unangetastet. Der Cluster-Build
  in `DrawScene` steht jetzt vor dem ersten `BindSceneLighting` und außerhalb von
  `HE_HAVE_SHADERC`; `BindSceneLighting` schreibt das Gate jedes Frame für alle drei
  Programme (Uniform-Werte bleiben pro Programm stehen). macOS-GL (4.1) unverändert.
- **Zeugen (lokal, M5, Release):** Rezept §3.1 mit `MANYLIGHTS=16builtin`. Vorher
  7/16 Lichtpools (der 8. Fensterplatz ist das Richtungslicht), nachher 16/16;
  außerhalb des neuen Pools pixelgleich (3 Pixel ±1 an der Überlappungskante).
  Bitgleich zu vorher: `HE_FORWARD_CLUSTER=0`, Deferred (`RENDERPATH=1`),
  Graph-Material (`MANYLIGHTS=16`), GL `16builtin` und GL `16` (macOS-GL 4.1).
  Rauschboden 0 (zwei Läufe md5-gleich). `MTL_DEBUG_LAYER=1` (nslog): 0 fehlende
  Bindungen in Forward, `HE_FORWARD_CLUSTER=0` und Deferred.
  `validate_embedded_shaders.py` linkt die 4.30-Fassung aller drei Programme
  (`-l`, VS + kUnlitFS); Gegenprobe: Tippfehler im Cluster-Zweig und dieselbe
  Fassung unter `#version 410` werden abgelehnt.
- **Offen:** Laufzeit auf echtem GL 4.3 (NN-WS03 o. ä.). Rezept wie §3.5 mit
  `16builtin`; im Log muss „scene program uses clustered lighting (GLSL 4.30)"
  (dazu skinned/instanced) stehen, keine Rückfall-Warnung.

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

- ~~Der **eingebaute** Forward-Shader von **Metal** (`fragmentMain`) und **GL** (`kUnlitFS`)
  bleibt beim 8er-Fenster.~~ Erledigt in Thema 123 Schritt 1, siehe §3.6 (GL nur ≥ 4.3).
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
