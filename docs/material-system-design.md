# Material-System — Design (Unreal-artig, Cross-Backend-Codegen)

Zielbild (User): Ein `Material` ist eine **Abstraktion über Shader**. Endzustand = **visueller,
node-basierter Editor wie in Unreal**, der Shader-Logik erzeugt, plus eine **vorgefertigte
Shader-Standard-Bibliothek**. Der User schreibt/übersetzt **nie** Backend-Shader — die Engine
sorgt dafür, dass jedes Material auf **allen Backends läuft und gleich aussieht**.

Deckt Masterplan-Lücke **3.2 Shader-Cross-Compile** ab.

---

## 1. Ausgangslage (verifiziert)

- **Alle Shader sind von Hand pro Backend** geschrieben: MSL eingebettet in `MetalRenderer.mm`,
  GLSL in `OpenGLRenderer.cpp`, HLSL in `D3D11/12Renderer.cpp`, GLSL→`.spv` via `glslc` für Vulkan
  (`src/HE_Rendering/shaders/*.glsl`). Kein geteilter Cross-Backend-Source.
- **`MaterialAsset.shaderPath` wird nie gelesen** (RenderPass.cpp:10-79 batcht, liest ihn nicht).
  Jedes Material zeichnet mit **einer** hartcodierten Unlit-PBR-Pipeline pro Backend
  (Metal `m_scenePipeline`, GL `m_unlitProgram`, D3D `kSceneHLSL`, Vulkan `scene.*.spv`).
  Konsumiert werden nur `texturePaths` + `baseColor/metallic/roughness/opacity/doubleSided`.
- **`ShaderAsset` (Quelle in `CHUNK_SRC`) ist ein Stub** — kein Renderer ruft `getShader()` im
  Render-Loop.
- **Kein Cross-Compile-Tooling** vorhanden (kein glslang/SPIRV-Cross/shaderc/DXC).
- **Günstig:** Uniform-Structs (`Uniforms`, `SceneUniforms`) sind bereits **byte-identisch std140
  über alle Backends**. Konsistenz-Shims existieren: `kMetalClipFix` (Z [-1,1]→[0,1],
  MetalRenderer.mm:2445), Y-Flip beim Sampling auf Metal/D3D.

**Fazit:** keine Alt-Last im Weg; das teuerste Konsistenz-Teilproblem (Uniform-Parität) ist gelöst;
Vulkans `glslc`-Pfad ist der halbe Beweis der Codegen-Idee.

---

## 2. Kernentscheidung: das Unreal-Modell (fixer Surface, Graph liefert Attribute)

Der Node-Graph ist **kein** Freiform-Shader. Er berechnet nur die **Material-Attribute** (Base Color,
Metallic, Roughness, Normal, Emissive, Opacity, …), die als Eingang in ein **festes Lighting-Modell**
der Engine fließen. Die Engine besitzt Vertex-Transform, Beleuchtung, Schatten, Fog, Tonemap.

Das ist die entscheidende Vereinfachung: Der Graph wird zu **einer generierten Funktion**

```glsl
MaterialAttributes evaluateMaterial(Varyings v);
```

die in ein **Uber-Shader-Template** je **Surface-Domain** (Lit / Unlit / Transparent / …) einge-
spliced wird. Dadurch bleibt Cross-Backend-Konsistenz **beherrschbar**: die Engine liefert überall
dieselbe Lighting-Mathematik; nur die Attribut-Funktion variiert.

**Vertex-Factory-Vertrag (wichtig):** Das Material erzeugt **nur die Fragment-Attribut-Funktion**.
Jeder Vertex-Pfad — statisch (`scene.vert`), skinned (`skinned.vert`), instanziert — muss **denselben
`Varyings`-Struct** füllen. So bleibt das Material **vertex-factory-agnostisch**; der Vertex-Pfad ist
eine orthogonale Dimension (siehe Varianten-Explosion, §7).

---

## 3. Pipeline (Autoring → alle Backends)

```
Node-Graph  ──emit──▶  kanonisches GLSL (evaluateMaterial)  ──splice──▶  Uber-Shader-Template (Domain)
                                                                              │
                                                             glslang ▼  (GLSL → SPIR-V)
                                                                          SPIR-V
                                    ┌───────────────┬──────────┴───────────┬───────────────┐
                            SPIRV-Cross▼        SPIRV-Cross▼           SPIRV-Cross▼      (direkt)▼
                                MSL (Metal)     HLSL (D3D11/12)     GLSL 4.1 / GLSL-ES     SPIR-V (Vulkan)
```

- **Kanonische Sprache: GLSL** (Vulkan-Pfad beweist ihn schon; glslang+SPIRV-Cross Standard).
- **Cross-Compile: glslang (GLSL→SPIR-V) + SPIRV-Cross (SPIR-V→MSL/HLSL/GLSL)** — der bgfx/Godot-Weg.
- **Cook offline:** Hash = (Graph/Template + Permutation + Domain). Beim Import/Cook alle Backend-
  Varianten kompilieren, in einen **Shader-Cache** legen (keyed by Hash+Backend), via **hpak** packen.
  Runtime lädt nur die Variante fürs aktive Backend (kein Runtime-Compile im Shipping).
- **Editor-Iteration:** in-process glslang+SPIRV-Cross → Live-Preview beim Editieren.
- **Konsistenz-Vertrag** in den Template-/Cross-Compile-Schritt gebacken: linearer Farbraum + sRGB
  an den Enden, Clip-Space (`kMetalClipFix`-Äquivalent als SPIRV-Cross-Option/Define), Y-Flip,
  Sampler-Defaults.

> **Verifizierbarkeits-Realität (ehrlich benennen):** „Gleiches Aussehen" entsteht **per Konstruktion**
> (identische Mathematik via geteilter Quelle + Cross-Compile), aber **visuell verifizieren** kann ich
> in dieser Umgebung nur **Metal** (headless HE_DUMP). GL läuft in der Sandbox nicht, D3D/Vulkan sind
> „blind" (Windows). Golden-Image-Parität kann daher nur **Metal** garantieren; auf den anderen Backends
> gilt „kompiliert + statische Checks bestehen", **nicht** gemessene Optik-Parität. Das begrenzt, was
> die „überall gleich"-Zusage (Entscheidung §6.6) real bedeuten kann — echte Parität auf GL/D3D/Vulkan
> braucht Verifikation auf echter Hardware durch den User.

---

## 4. Nötige Änderungen an Assets & Renderern

- **`MaterialAsset` weiterentwickeln:** von {shaderPath, PBR-Scalars, Texturen} zu
  {surfaceDomain, templateOrGraphRef, typed Parameter, typed Textur-Bindings, Permutations-Flags,
  (cooked) per-Backend-Shader-Hash}. Alt-Felder bleiben als „Standard"-Parameter erhalten.
- **`ShaderAsset` → erstklassig** oder neuer `MaterialGraphAsset` (Graph-Definition; Nodes+Edges,
  CBOR wie Prefab). Graph kompiliert zu kanonischem GLSL.
- **DrawCall trägt ein Pipeline/Shader-Handle** aus dem Material (statt der einen hartcodierten
  Pipeline). Jeder Backend bekommt einen **Shader-Cache** keyed by Material-Shader-Hash, der die
  Pipeline aus dem cross-kompilierten Source baut. Der bestehende PBR-Pfad wird zum „Default-Material".
- **Uniform-Parität nutzen:** die schon byte-identischen Structs sind Source-of-Truth für C++ **und**
  generierten Shader-Code (Material-Parameter als zusätzlicher std140-Block).

---

## 5. Phasierung (jede Stufe liefert eigenständig Wert)

**M0 — Cross-Compile-Backbone (dünner End-to-End-Durchstich):** glslang + SPIRV-Cross vendorn. Die
Kette an **einem neuen, trivialen** Material (Unlit/Emissive) **komplett auf Metal** beweisen:
Autoring → glslang → SPIRV-Cross → **material-gewählte Pipeline** → Render. Das validiert
Autoring→Compile→Cross→Select→Render am schnellsten und risikoärmsten.
→ **Nicht** als ersten Gate den bestehenden Scene-Shader neu schreiben — der ist der schwierigste
(sky/weather-injiziert zur Laufzeit) und ein bewegliches Ziel; seine Parität wird ein **späterer
Migrations-Check**, nicht das erste Tor. Gate ist **visuell** (SPIRV-Cross-Output ist kein Byte-Match
zu handgeschriebenem MSL — nur Optik zählt).
→ Deliverable: Offline-`shaderc`-Tool + In-Process-Compiler, Metal-Durchstich visuell verifiziert.

**M1 — Material-getriebene Pipelines:** Renderer wählen die Pipeline aus dem Material-Shader-Hash
(per-Backend-Shader-Cache) statt der einen hartcodierten. Default-Material = Uber-Shader aus M0.
Cook kompiliert+cached Varianten; hpak packt sie. Noch kein Authoring, aber Materials **können** jetzt
eigene Shader tragen.

**M2 — Surface-Template-Substrat (Std-Bib v1):** einige komplette Templates (Standard Lit, Unlit,
Transparent, evtl. Foliage) als kanonisches GLSL mit `#define`-Permutationen + typed Parametern.
Das ist der **Splice-Target-Layer** für den Graphen und zugleich die Parameter-Quelle für den
Simple-Modus. Noch keine Graph-UI — nur die Templates + Parameter-Reflection.

**M3 — Node-Graph-Editor = „Advanced-Modus" (priorisiert, User-Wunsch):** visueller Graph
(z.B. `imgui-node-editor`), dessen Nodes kanonische GLSL-Snippets emittieren → `evaluateMaterial`
→ splice in ein M2-Template → dieselbe M0/M1-Pipeline. Node-Std-Bib (Math, Texture, UV-Panner, Noise,
Fresnel, Blend, PBR-Inputs, Custom-Code-Node als Escape-Hatch). Das ist das primäre UI-Ziel.

**M3.5 — Simple/Advanced-Umschalter (Unreal Material vs. Material-Instance):**
- **Advanced = „Material"**: der volle Node-Graph (M3).
- **Simple = „Material-Instance"**: nur die vom Template/Graph **exponierten Parameter** + Texturen,
  kein Graph-Editing — die 90%-Alltagssicht mit Live-Preview-Kugel.
Beide kompilieren durch **dieselbe** M0-Pipeline; Simple ist eine Instanz eines Advanced-Materials.
Der **Material-Editor-Tab** (aus `editor-asset-tabs-plan.md`) hostet beide Modi.

**M4 — Konsistenz-Härtung & Ausbau:** Metal-Golden-Image-Tests + User-HW-Check der übrigen Backends,
mehr Nodes/Domains, Instancing/Perf, Hot-Reload.

---

## 6. Entscheidungen (mit User getroffen 2026-07)

1. **Graph-Modell:** ✅ **Unreal-Modell** — fixer Surface, Graph liefert nur Attribute.
2. **UI-Reihenfolge:** ✅ **Node-Editor zuerst (Advanced-Modus)**, danach **Simple/Advanced-Umschalter**
   (Simple = nur Parameter = „Material-Instance"; Advanced = Graph = „Material"). Siehe M3/M3.5.
3. **Nächster Schritt:** ✅ **M0-Spike** — glslang + SPIRV-Cross vendorn + Metal-Durchstich.
4. **Kanonische Sprache:** GLSL (empfohlen, noch offen) vs. HLSL.
5. **Cross-Compile-Stack:** glslang + SPIRV-Cross (empfohlen, noch offen) vs. shaderc.
6. **Scope:** erst User-Materials, Core-Shader später opportunistisch migrieren (empfohlen, noch offen).
7. **Compile-Timing:** offline Cook + Editor-Live-Compile (empfohlen, noch offen).

---

## 7. Risiken
- **Varianten-/Permutations-Explosion (beißt ab M1, nicht M4):** Material × Domain × Feature-`#define`s
  × **Vertex-Factory** (statisch/skinned/instanziert) × 5 Backends ist *der* klassische Skalierungs-
  Fehler von Material-Systemen. Erzwingt einen **Varianten-Key im `MaterialAsset`-Schema** und eine
  **Cook-Cache-Strategie ab M1** (nicht später nachrüsten). Vertex-Factory bewusst als orthogonale
  Dimension halten (§2-Vertrag), damit sie additiv statt multiplikativ mit dem Material wächst.
- **Verifizierbarkeit nur Metal:** siehe Kasten in §3 — GL/D3D/Vulkan-Optik-Parität ist in dieser
  Umgebung nicht messbar; braucht User-HW-Verifikation. Die „überall gleich"-Zusage gilt *per
  Konstruktion*, nicht *per Messung* auf 4 der 5 Backends.
- **glslang/SPIRV-Cross Build-Integration** (FetchContent, arm64/macOS, Größe) — M0-Aufgabe.
- **macOS GL 4.1**: SPIRV-Cross muss GLSL 410 (kein SSBO/Compute) treffen — Ziel-Profil sauber setzen.
- **Cross-Backend-Parität**: subtile Unterschiede (Präzision, Sampler, Clip-Space) — Metal-Golden-Image
  ab M0 mitziehen, nicht erst M4; übrige Backends per HW-Check.
- **Umfang M3**: der Node-Editor ist ein Feature für sich; erst bauen, wenn M0–M2 stabil sind.
```
