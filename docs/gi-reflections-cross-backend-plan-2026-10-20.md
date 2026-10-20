# GI-Reflektionen auf D3D11 / D3D12 / Vulkan: Pass-Design und Schnittstelle

Stand 2026-10-20, Thema 191 Schritt 1. Basis: `docs/gi-reflections-plan.md` (Metal P0–P5, GL-Port §10).
**Kein Shader-Code, keine Backend-Implementierung** in diesem Schritt.

## 0. Die Prämisse des Auftrags stimmt nicht

Der Auftrag klang nach einer neuen Abstraktion „Reflektions-Pass für D3D und Vulkan". Die
Abstraktion **gibt es schon und sie ist backend-neutral**; es fehlt nur die Umsetzung auf
D3D11/D3D12/Vulkan. Belege (`git grep`, alle auf diesem Branch):

| Teil | Ort | Stand |
|---|---|---|
| Einstellungen | `IRenderer::GIReflectionSettings` + `SetGIReflectionSettings` (`src/HE_Core/include/Renderer/IRenderer.h:585/613`) | in HE_Core, für alle Backends sichtbar |
| Capability | `Capabilities::supportsGIReflections` (`IRenderer.h:261`) | Metal `true`, GL `= m_giSupported`, D3D11/D3D12/Vulkan `false` (Default) |
| Persistenz/Editor/Game | `ProjectSettings.h:247-252` (`giReflection*`), `EditorSettingsPanel`, `EditorConfig.h`, `GameApplication.cpp` | fertig, backend-unabhängig |
| Shading-ABI | `Lighting::giRefl[4]` (`MaterialShaderLibrary.h:116`): x Intensität, y maxRoughness, z „Textur gebunden", w Auflösungs-Blur-Floor | geteilt |
| Verbraucher | `heLitP`-Kaskade Sky → GIRefl → SSR (`MaterialShaderLibrary.cpp:742-753`), Sampler `heGIReflFwd` = Binding 32 / `t32`+`s9` (`:318`, `:1466`) | geteilt; D3D12 (`D3D12MaterialRootSignature.h:35`) und Vulkan (`VulkanRenderer.cpp:9589`) belegen den Slot **schon**, mit Null/Weiß und `giRefl.z = 0` |
| Producer | Metal `EncodeGIReflections`, GL `RenderGIReflections` (`kGiReflCS`) | nur diese zwei |

Das Thema ist also **D3D/Vulkan-Parität**, nicht „neuer Pass von Null". Der Wortlaut „SSR + GI-Probes
als Fallback" beschreibt den Ist-Zustand ungenau: der GI-Reflektions-Pass ist ein **Strahl-Trace
gegen die GI-Beschleunigungsstruktur**, Treffer werden aus dem DDGI-Probe-Feld beleuchtet (Probes
sind das Hit-Shading, nicht ein Fallback), und SSR liegt als zweites Glied *darüber*.

## 1. Ansatz-Entscheidung: GL-Modell, nicht Metals Deferred-Composite

D3D11/D3D12/Vulkan folgen dem **GL-Port** (`gi-reflections-plan.md` §10), nicht Metals
`kSSRCompositeFS`-Pass. Gründe (alle geprüft):

- D3D/Vulkan-SSR ist **forward-only** (`IRenderer.h` Capability-Kommentar; `VulkanRenderer.cpp:1229ff`); es gibt
  keinen Deferred-SSR-Composite, den man erweitern könnte.
- Ihr Deferred-Resolve ruft dasselbe `heLitP` (Plan §10 „gratis mitversorgt"), die Kaskade liegt dort schon.
- Die Compute-Kette (G-Buffer-Prepass → Kernel gegen SW-BVH → Temporal → Blur) existiert für GI-Schatten auf allen drei:
  Vulkan `runGi` (`VulkanRenderer.h:1435`), D3D11/D3D12 `giShadowCS`/`giProbeCS` (`D3D11Renderer.cpp:3129`).
  `supportsGlobalIllumination` ist dort `true`/`giSupported`.
- Der Sampler-Cap (16 pro Material) bleibt unberührt, weil das Ergebnis über den schon reservierten Slot 32 kommt.

### Pass-Reihenfolge pro Frame (für beide Backends identisch)

```
updateGiAccel / UpdateGIAccel        (existiert; baut BVH auch wenn nur GIRefl an: m_giEnabled || reflEnabled)
GI-Prepass                           (existiert; NEU: roughness+metallic als zusätzliche Ausgabe)
[GI-Schatten / Probe-Update]         (existiert, nur wenn GI-Diffus an)
GIRefl-Trace-CS                      (NEU) rays = tier.rays, Auflösung = Prepass-Größe (siehe §3)
[GIRefl-Temporal]                    (NEU, optional; GL hat ihn per `if (false && …)` abgeschaltet)
GIRefl-Blur (giReflBlurReachFor)     (NEU) separabel, confidence-gewichtet
→ Textur an heGIReflFwd (t32 / Binding 32), Lighting.giRefl = {intensity, maxRoughness, 1, blurFloor}
Hauptpass (Built-in + heLitP)        Kaskade Sky → GIRefl → SSR (existiert, SSR forward bleibt oben)
```

Ergebnis-Textur: RGBA16F, rgb = Radiance, a = Confidence (0 = Miss, Composite behält den Sky-Term).
Disabled = Gate `giRefl.z = 0` → Bild byte-identisch zu „Feature nie existiert" (Vertrag aus `IRenderer.h`).

## 2. Schnittstelle (Stand nach diesem Schritt)

Neu geteilt: `src/HE_Rendering/include/HorizonRendering/GIReflectionTier.h` (`namespace HE`, header-only inline,
darum **kein `HE_API`**; reine Arithmetik wie `GIJitter.h`):

- `giReflTierFor(quality, blurEnabled) → {rays 1/2/4, resDiv 4/2/1, blurFloor 0.75/0.35/0, jitter}`
- `giReflBlurReachFor(maxRoughness, rays, resDivUsed) → {reach, levels, unit}`
- `kGIReflLobeScreenPx = 24` (vorher zwei Kopien in `MetalRenderer.h` / `OpenGLRenderer.h` mit „keep them together").

Metal und GL nutzen es jetzt (Inline-Rechnung ersetzt, Werte gleich; `tests/test_gi_reflection_tier.cpp`
pinnt die Metal-Tabelle und GLs `resDivUsed = 2`). **Dokumentierte Lücke GL:** GL traced mit fester Prepass-Auflösung
und übergibt `resDivUsed = 2`; `tier.resDiv`/`blurFloor` benutzt es nicht. D3D/Vulkan haben dieselbe Eigenschaft
(Prepass ist halbe Auflösung), dürfen dasselbe tun, müssen es dann in ihrem Handoff sagen.

**Material-Parameter (Roughness/Metallic):** laufen *nicht* über `SSRTraceUniforms` (das ist Metals SSR). Der Pass bekommt sie
(a) vom Empfänger per Prepass-Ausgabe, GL: 3. MRT `(roughness, metallic)`; D3D hat sie im Objekt-cbuffer (`uPBR`,
`HlslSources.h:748ff`), Vulkan über die Material-/Push-Daten des Prepass-Pipelines; (b) vom Treffer per Instanzdaten
(`baseColor.a = metallic`, `emissive.a = roughness`, Metal-/GL-Layout; Auflösung zentral in
`HE::giInstanceSurface`, `GiInstanceSurface.h`). `maxRoughness` fadet im Shading (`heLitP`, Forward), nicht im Pass.

**Bewusst kein neues virtuelles Pass-Interface:** `RenderPass` (`RenderPass.h`) wird nur von `RenderGraph` benutzt,
kein Backend hängt daran. Die Backends sind monolithisch; ein Interface nur für diesen Pass wäre die erste
Ausnahme ohne Nutzen. Die Daten-/Funktionsform (Settings in HE_Core + freie Funktionen) ist die vorhandene Konvention.

## 3. Lücken D3D11 / D3D12 / Vulkan (Grundlage für Schritt 2/3)

1. **Kernel fehlt** (`gi_refl.comp` / HLSL-`GiReflCS`): Portierung von `kGiReflCS` (`OpenGLRenderer.cpp`, GLSL 430).
   Traversal-Code hat HLSL-Fassung `kGiTraversalHLSL`, Vulkan `gi_shadow.comp`.
2. **Instanzlayout zu schmal:** Vulkan `GiInst` (`gi_shadow.comp:20`, 96 B) und D3D `GiInst`
   (`HlslSources.h:799`) haben nur `baseColor`; GL hat zusätzlich `emissive` und den Landschaftsindex
   (`OpenGLRenderer.cpp:1795`). `GIInstanceGpu.pad0/pad1` (`VulkanRenderer.h:1412`) sind frei, reichen aber nicht für
   emissive(vec4)+metallic/roughness. Layout-Wechsel = Shader + CPU-Struct + `static_assert` gleichzeitig.
3. **Prepass-Ausgabe:** Vulkan `gi_gbuf.frag` schreibt nur Pos+Normale (`oNorm.a = 0`); D3D `GiGBufPS` (`HlslSources.h:780-786`, gelesen) ebenso: `pos`+`norm` mit `norm.a = 0`; der `uPBR`-cbuffer ist im VS
   deklariert, wird aber nicht in den PS durchgereicht. roughness/metallic fehlen.
   Billigste Option: `oNorm.a`/`oPos` packen statt dritter Render-Target (Schritt 2/3 entscheidet, hier nur Vorschlag).
4. **Built-in-PBR-Shader haben keine Kaskade:** Vulkan `shaders/scene.frag` und D3D-HLSL-Scene-Shader (`HlslSources.h`) enthalten kein
   `giRefl`/`ssr`. Nur Graph-Materialien (`heLitP`) und der Deferred-Resolve sehen das Ergebnis, bis dort ein Gegenstück zu GLs `kUnlitFS` steht.
5. **Bindung:** Vulkan Binding 32 und D3D12 `t32` sind belegt (Weiß/Null), müssen die echte Textur + `giRefl.z = 1` bekommen.
   **D3D11:** `grep heGIReflFwd|t32|t31 D3D11Renderer.cpp` findet den Slot nirgends (Kommentare zu `heSSRFwd` ausgenommen);
   ob D3D11 an `t32` etwas bindet, ist damit **nicht belegt**, Schritt 2 liest die Material-SRV-Bindung dort selbst (D3D12 deklariert den Slot; die geteilte `kHlslMaterialPins`-Tabelle pinnt ihn auf `t32/s9`).
6. **Vulkan ohne Sky-Cube:** `fog.z` wird nie gesetzt, die Kaskade startet bei Schwarz (Kommentar `MaterialShaderLibrary.cpp:721`).
   Ein Miss (`a = 0`) zeigt dann Schwarz statt Himmel, nicht Fehler des Passes.
7. **Vulkan HW-Ray-Query:** `gi_*_hw.comp` existieren für Schatten/Probes. Für Reflektionen braucht ein Closest-Hit
   Dreiecksindex + Vertexdaten (Metal: Argument-Buffer, GL: `giHitNormal` aus SW-BVH). Empfehlung: **Schritt 3 startet mit dem SW-Kernel**
   (SSBOs werden ohnehin immer hochgeladen), HW später.
8. **Bounces:** `GIReflectionSettings::bounces` ignoriert GL; D3D/Vulkan folgen GL (1 Segment) und dokumentieren das.
9. **Terrain/Skinned** nicht in der BVH, Landschaft über `GiLandscape` (nur GL/Metal-Kernel kennen sie): identische Lücke wie GI-Diffus.

## 4. Fallen für die Folge-Schritte

- **Drift-Test:** `tests/test_culling.cpp:2219` (Subcase „DDGI probe sampling on the shading side", nicht mehr `:1114`) schneidet
  `kGiReflCS` per Literal-Namen aus `OpenGLRenderer.cpp`. Nicht umbenennen. Jede neue Kopie von `sampleDDGIIrradiance`
  (HLSL, Vulkan-`.comp`) in diesen Test aufnehmen, sonst Drift unbemerkt.
- **Neue geteilte Konstanten** brauchen `// SYNC:`-Marker, sobald sie in Shadertext stehen. `GIReflectionTier.h` ist reine CPU-Seite
  (eine Definition), darum dort kein Marker.
- **Vulkan-Submits** nur über `submitGraphics`/`deviceWaitIdle`; neue Pipelines über `createGraphicsPipelines`/`createComputePipelines`.
- **D3D-Startup:** neue eager `timedD3DCompile` auch in `prefetchPipelineShaders()` eintragen (sonst serieller Start, Lehre aus Thema 200).
- `scripts/he_vk_imagetests.py:204-207`: Fall `gi_refl` ist **report-only**, weil Vulkan off == on liefert; in Schritt 3 zum beurteilten Paar machen.
- GLSL 4.10 / HLSL SM5.0: `glslangValidator -D -V -e` für HLSL (Memory „HLSL offline prüfen"), GLSL-Strings werden vom C++-Build nie kompiliert.

## 5. Verifikation

**Geprüft:** Premise per `git grep` über `src/ tests/ docs/ scripts/`; Tier-Test `test_gi_reflection_tier.cpp` standalone
(doctest, 5 Fälle / 51 Assertions grün; Negativkontrolle `0.75 → 0.70` macht ihn rot); `MetalRenderer.mm` und `OpenGLRenderer.cpp`
`-fsyntax-only` mit den Compile-Flags der Haupt-Checkout-`compile_commands.json` gegen die Worktree-Quellen (rc 0, Negativkontrolle mit
umbenanntem Symbol schlägt an).
**Nicht geprüft:** vollständiger `he_tests`-Build und `ctest -R test_culling` (kein Build-Baum im Worktree; Schritt berührt keinen
Shadertext, also kein Effekt auf den Drift-Test erwartet); Pixel-A/B der Metal-/GL-Umstellung (die Rechnung ist durch den Test
auf die alten Werte gepinnt, Bildvergleich nicht gelaufen); D3D/Vulkan-Rendering (Mac, nichts davon verifizierbar).

## Handoff

**Erledigt (Schritt 1):** Prämisse geklärt (§0); Ansatz = GL-Modell (§1); geteilter Tier-Header
`HorizonRendering/GIReflectionTier.h` + Test + Umstellung Metal/GL; Lückenliste §3.
**Entschieden:** kein neues virtuelles Pass-Interface; `GIReflectionSettings`/`supportsGIReflections`/`giRefl[4]` bleiben die Schnittstelle;
D3D/Vulkan folgen GL, nicht Metals Composite. **Nur vorgeschlagen:** roughness/metallic in `oNorm.a` packen; SW-Kernel vor HW.
**Nächste Schritte in Reihenfolge:**
1. Schritt 2 (D3D11+D3D12): Prepass-Ausgabe erweitern, `GiInst` auf Metal-Layout (§3.2), HLSL-`GiReflCS` aus `kGiReflCS` portieren,
   Blur/Temporal mit `GIReflectionTier.h`, `t32` binden, `giRefl`-Felder füllen, `supportsGIReflections` anheben, prefetch eintragen.
2. Schritt 3 (Vulkan): `gi_refl.comp` SW zuerst, Binding 32 echt, Built-in-`scene.frag`-Kaskade, `he_vk_imagetests.py` `gi_refl` beurteilen.
3. Beide: `test_culling`-Drift-Subcase um die neuen Kopien erweitern.
**Fallen:** siehe §4. Kein Build-Baum im Worktree (Vollbau ~17+ min, CLAUDE.md/Memory: compdb + `-fsyntax-only` reicht für Mac-Syntax).
