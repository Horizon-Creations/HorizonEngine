# A4 — Material-Node-Graph-Shader auf D3D11/D3D12/Vulkan: Implementierungs-Spec

Vollständige, ausführungsreife Spezifikation (Design abgeschlossen; Codegen existiert bereits). Ziel:
Graph-Materialien (Material-Node-Editor) rendern auf D3D11/D3D12/Vulkan **identisch zu GL/Metal**. Aktuell
rufen diese drei Renderer `MaterialShaderLibrary` **nie** auf → Graph-Materialien rendern dort gar nicht.

## Was schon da ist (kein Neubau nötig)
- **`MaterialShaderLibrary`** (`src/HE_Rendering/…/material/`) macht die komplette Cross-Compilation via
  glslang→SPIRV-Cross. `enum Backend { Metal, HLSL, GLSL410, GLSLES300, SpirV }`.
  - `resolveShaders(cm, materialId, hash, fragOut, vertBodyOut)` → true, wenn das Material einen Graph hat.
  - `standardVertex(Backend)` / `customVertex(hash, body, Backend)` → Vertex-`Compiled`.
  - `fragment(hash, glsl, Backend)` → Fragment-`Compiled`.
  - `Compiled { bool ok; std::string source; std::vector<uint32_t> spirv; std::string log; }`
    — **SpirV-Backend füllt `.spirv`** (direkt an `vkCreateShaderModule`), **HLSL-Backend füllt `.source`**
    (HLSL-Text → `D3DCompile`).
- **A1/A2** haben in allen drei Backends bereits den Mesh-/Material-Textur- + Invalidate-Pfad → Textur-
  Descriptor/SRV-Erzeugung wiederverwenden für `heTex0`/`heTexP0..3`.

## Kanonisches Binding-Layout (set = 0) — GILT FÜR ALLE BACKENDS
Aus `MaterialShaderLibrary.cpp` (kLightingPreamble, buildCustomVertex) + `MaterialGraph.cpp:937-942`:

| binding | Typ | Name | Stage | Inhalt |
|---|---|---|---|---|
| 0 | UBO | `HeLighting` | Fragment | `MaterialShaderLibrary::Lighting` (64 B): sunDir+time, sunColor, ambient, camPos |
| 1 | UBO | `U` | Vertex | `{ mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr; }` (per-Objekt) |
| 2 | sampler2D | `heTex0` | Fragment | Legacy/Mesh-Textur |
| 3 | UBO | `HeParams` | Fragment | `{ vec4 v[16]; }` (256 B) — Graph-Parameter |
| 4..7 | sampler2D | `heTexP0..3` | Fragment | Graph-Projekt-Texturen |

- **Vulkan (SpirV):** Descriptor-Bindings = **exakt** obige (set 0, binding 0-7). Deterministisch, kein Risiko.
- **D3D (HLSL via SPIRV-Cross):** SPIRV-Cross-HLSL-Register ZUERST verifizieren — `src/HE_Tools/ShaderCompiler/
  ShaderCompiler.cpp` prüfen, ob es `binding`→`register`-mapping erhält (Default SPIRV-Cross: UBO binding B →
  `register(bB)`, sampler2D binding B → `Texture2D register(tB)` + `SamplerState register(sB)`). Root-Sig /
  Input-Layout danach ausrichten. **Nicht raten — den generierten HLSL-Header einmal ausgeben und die Register
  ablesen** (z. B. `fragment(...).source` loggen).

## Referenz-Pfad (GL, exakt zu spiegeln)
`OpenGLRenderer::getOrBuildMaterialProgram` (Zeile ~2989) + Draw-Integration (~5900-6000):
1. `resolveMaterialShader(dc.materialAssetId, key, frag, vert)` → Graph-Material? (mirror: `resolveShaders`)
2. Per-Material-Pipeline/PSO aus `standardVertex(BE)` + `fragment(key, frag, BE)` bauen, **gecacht per `key`**
   (wie `m_materialPrograms`). Precompiled-Variante (`MaterialAsset::precompiledShaders`) bevorzugen, sonst
   Laufzeit-Cross-Compile.
3. **`U`-UBO** füllen: `mvp=viewProj*dc.transform; model=dc.transform; color=vec4(baseColor,1);
   flags=vec4(hasTex,0,0,0); pbr=vec4(metallic,roughness,opacity,0);`
4. **`HeLighting`-UBO** (einmal/Frame): sunDir.xyz + sunDir.w=Zeit(s) (`HE_SKY_TIME`-Override beachten für
   headless), sunColor=`GetEnvironment().sunColor`, ambient=`m_renderWorld.ambient`, camPos=Kamera-Pos.
5. **`HeParams`-UBO**: aus `dc.paramOverride` (per-Entity, gewinnt) sonst `MaterialAsset::shaderParamData`
   (bis 64 float = 16 vec4), 0-gepaddet.
6. **Texturen**: `heTex0` = Mesh-/Material-Textur (A1-Pfad), `heTexP0..3` = Graph-Projekt-Texturen des
   MaterialAsset. Fehlende → 1×1-weiß-Default (wie A1-Null-Slot).
7. Draw. **Opazität/Blend:** `blendMode==2` (Translucent) erzwingt den Transparent-Pass (opacity ≤ 0.998).

## Per-Backend-Plan (Reihenfolge: Vulkan → D3D12 → D3D11)
**Vulkan zuerst** (deterministische Bindings):
- Per-Material `VkPipeline`-Cache (key = shader-hash). Module aus `.spirv` (VS+FS) via `vkCreateShaderModule`.
- **1 Descriptor-Set-Layout** für Set 0 (b0 UBO FS, b1 UBO VS, t2 sampler FS, b3 UBO FS, t4-7 sampler FS).
  Pipeline-Layout = dieses eine Set (+ ggf. Push-Constants ungenutzt).
- Per-Frame: `HeLighting`-UBO (1×), `U`-UBO-Ring (per-Draw), `HeParams`-UBO (per-Material). Descriptor-Pool
  + per-Material/-Draw Descriptor-Sets (oder dynamische UBO-Offsets wie beim bestehenden `m_frameUBO`).
- Vertex-Input = derselbe wie scene.vert (binding 0: pos/normal/uv, 32 B). Der Library-Standard-Vertex liest
  Attribute location 0/1/2 (non-Metal-Pfad, `buildCustomVertex(ssbo=false)`).
- Draw-Loop-Integration im opaken + transparenten Pass: wenn `resolveShaders` true → per-Material-Pipeline +
  Sets binden statt der Standard-Scene-Pipeline.

**D3D12** danach: per-Material-PSO (VS+PS aus `D3DCompile` der HLSL). Root-Sig an SPIRV-Cross-Register
ausrichten (b0/b1/b3 CBV, t2/t4-7 SRV + s2/s4-7 Sampler — VERIFIZIEREN). CBV-Ring für U/HeLighting/HeParams,
SRV-Descriptor-Table für die Texturen (bestehende `sceneSrvHeap`-Region wie A1). **D3D11** analog (VS/PS-
Objekte + `*SetConstantBuffers`/`*SetShaderResources`).

## Verifikation
- CI: kompiliert alle drei (+ glslc/D3DCompile der generierten Shader). **Compile ≠ Optik.**
- **Adversarielle Review** (wie A3) auf: Binding/Register-Match, UBO-std140-Layout (Lighting 64 B, U-Block,
  HeParams 256 B), Textur-Slot-Zuordnung, Draw-Integration (opak+transparent), Regressionsfreiheit der
  Nicht-Graph-Materialien.
- **B3 (Windows-GPU, Pflicht):** Graph-Material an ein Mesh → muss identisch zu GL/Metal aussehen; Parameter
  live ändern → sofortiges Update; Graph-Texturen korrekt. Validation-Layer sauber.

## Risiko-Hinweis
Greenfield-Subsystem × 3 blinde Backends. Empfehlung: **ein Backend fertig + reviewen + (idealerweise) auf HW
prüfen, BEVOR die anderen zwei folgen** — sonst multipliziert sich ein Binding-/Layout-Fehler über alle drei.
