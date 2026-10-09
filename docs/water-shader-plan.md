# Wasser-Shader als Engine-Material — Plan (Thema 152)

Stand 2026-10-05, Schritt 1 (Bestandsaufnahme), vollständig. Nichts am Renderer geändert; Abschnitt 3 nennt Renderer-Fehler, die spätere Schritte beheben müssen.
Schritt 2 (Material gebaut): §5a. Schritt 3 (Backend-Parität, GL-Fehler 1 behoben): §8.
Schritt 4 (Bilder auf Metal und GL, Tooltips, Handbuch, Stand 2026-10-06): §9.
Schritt 7 (Urteil „sieht sehr comichaft aus", realistischeres Aussehen, Stand 2026-10-09): §11. Die
Defaults in §5a und §9.2 sind seitdem überholt; gültig ist die Tabelle in §11.2.

Ziel: ein Wasser-Material als Engine-Content, gebaut als Material-Node-Graph, auf allen
fünf Backends über die bestehende Graph-Shader-Pipeline, mit vollem Parametersatz im
Inspector. Nicht Teil: Simulation, Physik/Schwimmen, Unterwasser-Postprocessing,
Ozean-Mesh-Komponente.

---

## 1. Befund: wie Default-Materialien registriert werden

Zwei Mechanismen gibt es heute:

1. **Im Code, `mem://`:** `ContentManager::initDefaultAssets` (`ContentManager.cpp:2964`)
   registriert Würfel, Quad, Weiß-Textur, `kDefaultMaterialId` (`:3106`, reines PBR ohne
   Graph), `kDefaultTerrainMaterialId` (`:3157`) und die Editor-Icon-Materialien
   (`:3243–3269`, als **Graph** gebaut: `MaterialGraph` → `generateFragment` →
   `nodeGraphJson`/`customShaderFragGlsl`). UUIDs in `DefaultAssets.h`.
2. **Als Datei, `Engine/…`:** `.hasset` unter `EditorDeps/EngineContent/`, deterministisch
   erzeugt und committet von Generatoren außerhalb des normalen Builds
   (`src/HE_Tools/CMakeLists.txt:136–173`): `mesh_gen` (UUID-Block hi=0x100), `widget_gen`
   (0x200), `matfn_gen` (0x300, `src/HE_Tools/src/MatFnGen/main.cpp`). **hi=0x400 ist frei.**
   `scanContentDirectory` indexiert sie mit Präfix `Engine/` (`ContentManager.cpp:2907–2928`),
   der Exporter packt sie mit (`ProjectExporter.cpp:778–783`). Für Nutzer schreibgeschützt,
   Speichern legt eine projekteigene Kopie an (`ContentManager.cpp:983–990`).

Was davon im Editor auftaucht:

- **Inspector-Materialslot** (`InspectorPanel.cpp:1965–1973`) und Picker
  (`EditorWidgets.cpp:91–199` über `HcEditorUtil::listAssets`, `HcEditorUtil.cpp:93–100`)
  listen nur `.hasset` auf der Platte unter Projekt- und Engine-Root. **`mem://`-Materialien
  erscheinen nie** — nicht absichtlich versteckt, sie haben schlicht keine Datei.
- **Content Browser** ist rein dateisystembasiert (`GlobalState::refreshEngineFolder`,
  `GlobalState.cpp:820–879`), also dasselbe.
- Ein `.hasset`-Material wird beim Laden aus `nodeGraphJson` neu erzeugt
  (`regenerateMaterialFromGraph`, `ContentManager.cpp:764`; Fragment, G-Buffer, WPO,
  Texturen, Parameter, Werte bleiben per Name erhalten). Der Generator muss nur den Graphen
  schreiben.
- Es gibt **noch kein** Material-`.hasset` in EngineContent (`Materials/` hat nur
  `.gitkeep`); der Pfad `Engine/Materials/…` ist in Tests schon vorgesehen
  (`tests/test_mcp_tools_material.cpp:910–935`).

Das „globale Fallback-Material" ist **nicht** `kDefaultMaterialId`: ein Mesh ohne Material
behält `materialAssetId` null (`RenderExtractor.cpp:131–133`) und zeichnet mit dem fest
eingebauten Backend-Default (Metal `MetalRenderer.mm:9107–9122`, `:13344`: weiß bei Textur,
sonst Grau 0.55, Rauheit 0.5; OpenGL gleich, `OpenGLRenderer.cpp:8404`; D3D/Vulkan nicht
geprüft). `kDefaultMaterialId` nutzen nur die Projektvorlagen (`ProjectManager.cpp:1631`,
`:1637`, `:1661`), die Vorschau im LevelScriptPanel (`:2498`) und `TerrainSystem`, das ihn als
„nicht zugewiesen" liest und durch das Terrain-Material ersetzt (`TerrainSystem.cpp:417`,
`:490`). Ein Test pinnt seine Werte (`tests/test_contentmanager.cpp:889–899`).

---

## 2. Befund: was das Material-Graph-System lesen kann

`docs/material-system-design.md` beschreibt den Plan (M0–M4); der heutige Stand steht im
Code: `src/HE_Core/include/MaterialGraph/MaterialGraph.h` (Knotenliste, Grenzen),
`src/HE_Core/src/MaterialGraph/MaterialGraph.cpp` (Codegen) und
`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp` (Lighting-Preamble `heLitP`).
Der Graph liefert nur die Attribute (Unreal-Modell), die Beleuchtung macht die Engine.

| Fähigkeit | Stand | Beleg |
|---|---|---|
| **Zeit** | ja: Knoten `Time` = `heLight.sunDir.w`, `Panner` = UV + Speed·Zeit. Geht im Fragment **und** in der WPO-Vertexstufe | `MaterialGraph.cpp:605`, `:730`; `MaterialShaderLibrary.cpp:858` |
| **Himmelsumgebung / Spiegelung** | indirekt ja: kein eigener Knoten, aber `heLitP` sampelt die Himmels-Cubemap `heSkyEnv` (Binding 15) mit nach Rauheit gebogenem Reflexionsvektor und rauheitsbewusstem Schlick-Fresnel, danach die Forward-Kaskade GI-Reflexion → SSR. Ein **lit** Material mit niedriger Rauheit spiegelt den Himmel also ohne eigenen Knoten. Gate: `heLight.fog.z` (Cubemap gebunden) | `MaterialShaderLibrary.cpp:249`, `:675–716` |
| **Fresnel** | ja: Knoten `Fresnel` (`pow(1-N·V, p)`, echter Blickvektor pro Pixel), dazu `ViewDir`, `CameraPos`, `CameraDistance` | `MaterialGraph.h:47`, `:51`, `:77–78` |
| **Wellen** | Bausteine da: `Panner`, `Noise`, `FBM Noise`, `Noise Texture`, `Sine`, `WorldPos`, `Normal Map` (Tangentenraum → Welt ohne Vertex-Tangenten), WPO-Pin (Vertex-Wellen) | `MaterialGraph.h:38–60`, `:110`, `MaterialGraph.cpp:614` |
| **Szenentiefe** | **nein.** Kein Knoten, die Material-Preamble deklariert keine Tiefentextur. `heGBDepth` gibt es nur in Deferred-Resolve/SSR-Shadern, nicht im Material-Fragment | `MaterialShaderLibrary.cpp:1119–1122`, `:1441` |
| **Szenenfarbe (Refraktion)** | **nein** für Surface-Materialien. Einziges Vorbild ist der UI-Knoten `Backdrop` (nur Domain UserInterface, sonst Schwarz). `heSceneColor` existiert nur im SSR-Trace | `MaterialGraph.h:138`, `MaterialShaderLibrary.cpp:1439` |
| **Blend-Modi** | `Opaque` / `Masked` / `Translucent` am Output-Knoten (`p[1]`); Translucent geht in den sortierten Alpha-Blend-Pass | `MaterialGraph.h:174` |
| **Kantenglättung des Glanzes, Wetter** | `heLitP` macht Specular-AA und Wetter (nass/Schnee) automatisch, auch auf dem Wasser (Schnee auf Wasser per Normale y → später ggf. abschalten) | `MaterialShaderLibrary.cpp:640–661` |

**Harte Grenzen** (`MaterialGraph.h`):

- `kMatMaxParams = 16` vec4-Slots im `HeParams`-UBO. **Falle:** der 17. Parameter wird
  nicht abgelehnt, sondern still als Literal eingebacken (`paramSlot` gibt -1,
  `MaterialGraph.cpp:524–535`), er taucht dann im Inspector einfach nicht auf. Der
  MCP-Pfad prüft das (`McpToolsMaterial.cpp:1206`), der Generator muss es selbst prüfen.
- `kMatMaxGraphTextures = 4`, dedupliziert nach Pfad: eine Normalmap, die von drei
  Pannern gelesen wird, kostet einen Slot. Über dem Budget fällt der Knoten auf `heTex0`.
- Die Engine-Plane `Meshes/Plane.hasset` ist **ein einzelnes Quad**
  (`MeshGen/main.cpp:114`). WPO-Wellen bewegen darauf nichts; die sichtbare Animation
  muss über Normalen/UV laufen, echte Vertex-Wellen brauchen ein unterteiltes Mesh.
- UVs der Plane laufen 0..1 über die ganze (skalierte) Fläche. Wellen deshalb aus
  `WorldPos.xz` ableiten (`Combine RGBA(x, z, 0, 0)` → Vec2), sonst strecken sie sich mit
  der Plane-Skalierung.
- Material-Instanzen (Eltern-Material + überschriebene Parameter/Static Switches) gibt es
  (`ContentManager::syncMaterialInstance`), Varianten wie "See" / "Ozean" können also
  Instanzen werden statt eigener Graphen.

---

## 3. Befund: Transparenz/Blend pro Backend

Alles aus dem Code gelesen, nichts auf Hardware gelaufen. Abkürzungen: M =
`Backends/Metal/MetalRenderer.mm`, G = `Backends/OpenGL/OpenGLRenderer.cpp`,
D11/D12 = `Backends/D3D11/D3D11Renderer.cpp` / `Backends/D3D12/D3D12Renderer.cpp`,
V = `Backends/Vulkan/VulkanRenderer.cpp`, MSL = `material/MaterialShaderLibrary.cpp`
(alle unter `src/HE_Rendering/src/`).

**Weg ins Blend-Pass (alle fünf gleich):** einsortiert wird nach Deckkraft, nicht nach
`blendMode`. `blendMode == 2` kappt die Deckkraft auf 0.998 (`MaterialScalars.cpp:21`, Metal
M:9118, GL G:8415), `RenderSorter::isTransparent` (< 0.999, `RenderSorter.h:18,28`) legt den
Draw dann in die Transparent-Liste. Ein Translucent-Graph landet also auch bei Opacity 1
immer dort.

| | Metal | OpenGL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|---|
| Translucent-**Graph** mit eigenem Shader | ja, `EncodeScene` M:14081–14143, PSO aus `GetOrBuildMaterialPipeline(blend=true)` M:12212 | ja, G:12173–12239 (`t.matProg`) | ja, `drawDC` D11:6573–6792, Schleife :6947 | ja, `drawDC12` D12:10132–10314, Schleife :10590 | ja, `drawDCVk` V:7045, Schleife V:7466 |
| Sortierung | hinten→vorn nach Objektursprung, pro Instanz, nicht stabil (M:14085) | hinten→vorn pro Instanz (G:12175) | hinten→vorn nach Ursprung (D11:6542) | dto. (D12:10098) | dto. (V:7029) |
| Blend | SrcAlpha / 1−SrcAlpha, nicht vormultipliziert | dto. (G:12178) | dto. (D11:4206–4216) | dto., im PSO (D12:8688–8703) | dto., Alpha ONE/ZERO (V:2946–2955) |
| Depth | Test LessEqual, Write aus (M:7220) | Test LESS, Write aus (G:12180) | LESS, Write aus (D11:4196) | LESS, Write aus | LESS, Write aus (V:2938) |
| Culling | keins (nie gesetzt) | keins | CULL_NONE (D11:4218) | CULL_NONE (D12:8686) | CULL_NONE (V:2932) |
| `sunDir.w` = Zeit | ja (M:14167) | ja (G:11106) | ja (D11:6063) | ja (D12:9620) | ja (V:6842) |
| `fog.z` / Himmels-Cube | ja (M:14245) | ja im Fill (G:11121), **aber siehe Fehler unten** | ja, t15 (D11:6155, :6702) | ja (D12:9708, :10229) | ja, Binding 15 (V:6919, :7266) |
| Forward-SSR (`ssr.x`) | nur Forward-Pfad (M:14178); deferred 0 | nur Forward (G:11148); deferred aus | 0, absichtlich (D11:6137) | 0, nie gesetzt | ja (V:6908) |
| Deferred-Pfad | ja; Translucent danach forward in `m_hdrColor`, vor Post/TAA (M:13621, :16365) | ja; dto. in `m_hdrFBO` (G:12173 nach Resolve) | keiner | keiner | keiner (V:6903) |
| Szenenfarbe/-tiefe lesbar im Translucent-Pass | **nein**: rendert in `m_hdrColor`/`m_hdrDepth`; nur Deferred-Zweipass hat `m_gbDepth` lesbar (nur G-Buffer-Opaque) | **nein**: rendert in `m_hdrFBO`; deferred `m_gbDepthTex` lesbar (nur G-Buffer-Opaque) | **nein**; Tiefen-SRV existiert (`viewportDepthSRV`), braucht aber Unbind/Copy wie bei Decals (D11:4809) | **nein**; Tiefen-SRV für Decals, braucht Zustandswechsel/Copy (D12:8970) | **nein**; ein einziger Render-Pass mit Sky…Translucent…Skinned (V:3862, :744–746) |

Auf Vulkan und D3D12 läuft die ganze Transparent-Schleife nur, wenn die **eingebaute**
Transparent-Pipeline existiert (V:7468, D12:10590). Fehlt sie, fallen auch Graph-
Translucents still weg.

`doubleSided` wird auf **keinem** Backend gelesen, alles ist zweiseitig. Für eine
Wasserfläche passt das. Die einzige vorhandene Farbkopie ist überall die SSR-History
(`CaptureSSRColorHistory`): das **vorige** Bild **mit** Transparenz, und nur bei aktivem
SSR. Für Refraktion taugt sie nicht (das Wasser sähe sich selbst).

**Fehler und Fallen, die das Wasser direkt treffen:**

1. **OpenGL forward: der Translucent-Graph bekommt kein frisches `HeLighting`.** Der
   Translucent-Zweig bindet `m_matLightUBO` nur (G:12205), er lädt ihn nie hoch. Hochgeladen
   wird nur im Deferred-Pfad (G:11204) oder im **opaken** Custom-Material-Draw (G:11582,
   Dedupe-Flag zurückgesetzt bei G:11189). Ist das Wasser das einzige Graph-Material im
   Bild, liest es den Stand vom Vorbild, vom UI-Pass (G:7523, `fog.z = 0`) oder von der
   Material-Vorschau (G:8578, `sunDir.w = 0` → **keine Animation**). Die Units 13–17
   (Weightmap, Himmels-Cube, AO, DDGI) bindet ebenfalls nur der opake Zweig
   (G:11641–11661). Selbst nachgelesen. Ohne Fix ist das Wasser auf GL forward im
   Zweifel still und spiegelt den Himmel nicht. Das ist eine Renderer-Änderung (nicht in
   diesem Schritt).
   **Behoben in Schritt 3** (§8): der Zweig lädt `HeLighting` jetzt selbst hoch und bindet
   13–17 wie der opake.
2. **GL: keine Sampler-Grenze geprüft.** Material-Programme nutzen Units bis 20 (G:3777–3826).
   macOS-GL 4.1 meldet typischerweise 16 Fragment-Units. Nichts im Code fragt
   `GL_MAX_TEXTURE_IMAGE_UNITS` ab. Ungeprüft, ob das heute stört; neue Sampler würden es
   verschärfen.
3. **Metal und GL: Graph-Texturen bleiben auf Slot 1–4 liegen** (M:14119–14124,
   G:12211–12215). Ein **eingebautes** transparentes Material, das danach kommt, liest dort
   CSM/Cube/AO und bekommt die falschen Texturen. Ungeprüft zur Laufzeit.
4. **Vulkan: Skinned-Meshes werden nach den Transparenten gezeichnet** (V:7481). Eine
   Figur hinter dem Wasser malt sich über die Wasserfläche.
5. **Deferred (Metal/GL): Wasser bekommt keine SSR**, weil das Deferred-Reflexions-
   Composite vor dem Forward-Schwanz läuft (M:16318) bzw. SSR deferred aus ist (G:10825).
   Es spiegelt dann nur den Himmels-Cube.
6. **SSAO auf Wasser** (Schluss, nicht getestet): bei `fog.w = 1` multipliziert `heLitP` das
   Ambient mit dem AO des opaken Grunds hinter dem Pixel (MSL:727–738).
7. Der Kommentar MSL:675–680 („Vulkan setzt `fog.z` nie") ist **veraltet**; Vulkan setzt es
   heute (V:6919, Fix dokumentiert V:2651).

**Was SceneColor/SceneDepth kosten würden (für das Renderer-Thema):**

- **Schnitt im Pass:** vor der Transparent-Schleife Pass beenden, Farbe und Tiefe kopieren,
  mit Load weitermachen. Vorbild ist `Backdrop` im UI-Pass: GL `snapshotBackdrop`
  (`glCopyTexSubImage2D`, G:7445–7484), Metal `cutForBackdrop` (M:12907–12925). Vulkan
  braucht eine Load-Variante von `m_postFxSceneRP` (Load/Store-Ops brechen die
  Pipeline-Kompatibilität nicht, V:3870); `CaptureSSRColorHistory` (V:11951) ist die
  Vorlage für Kopie + Barrieren. Vulkan und D3D haben im UI-Pass gar keinen
  Backdrop-Pfad, also auch kein Vorbild für die Kopie.
- **Bindungsnummern:** 34 = Szenenfarbe, 35 = Szenentiefe sind in MSL und MaterialGraph
  frei. 19–23 und 27–30 sind im Material-Fragment frei, aber in Geschwister-Shadern
  (Resolve, SSR, Decals) belegt, und `heSceneColor` ist als Name im SSR-Trace vergeben.
- **Sampler sind der Engpass, nicht Bindungen:** Metal hat 16/16 Sampler belegt
  (MSL:2500–2563; freie Texture-Slots 17–30 nur mit Inline-Constexpr-Sampler wie Binding 33,
  MSL:1311). HLSL SM5 hat 16/16 s-Register belegt (`kHlslMaterialPins` MSL:2639–2659). Also
  per `texelFetch` lesen (HLSL `Load`), den toten Sampler auf ein belegtes Register legen wie
  heAO→s0. **Folge:** Refraktion nur punktgesampelt, ohne Bilinear.
- **Stellen pro Backend:** Vulkan `VulkanMaterialLayout.h` `kBindings` (vor den
  Cluster-Zeilen) + Draw-Writes V:7196–7327 (`static_assert` V:7308) + Reflexions-Test
  `tests/test_material_graph.cpp:3115+`. Metal-Pin-Tabelle MSL:2500ff. HLSL-Pins +
  `D3D11MaterialBindings.h` + `D3D12MaterialRootSignature.h` (`kSrvPerDraw` 17→19, neue
  Range, Null-Views D12:8366). GL Units per Name in `setupProgram` (G:3777ff., ab 21 frei,
  siehe Falle 2). Codegen: Sampler nur deklarieren, wenn ein Knoten sie nutzt (wie
  `heLandscapeWeights`, MaterialGraph.cpp:1214).
- **Screen-UV:** `gl_FragCoord.xy / heLight.giParams.xy` wie `heSSRFwd` (MSL:704). Da die
  Kopie das eigene Render-Target ist, sollte der Ursprung überall passen (begründet,
  nicht getestet).

Nebenbefund (nicht Wasser): das Vulkan-Layout hat **kein Binding 14**, der Codegen
deklariert dort `heLandscapeWeights` (MaterialGraph.cpp:1220). Das ist Thema 143.

---

## 4. Entscheidung

**Das Wasser wird ein zusätzliches Engine-Material, es ersetzt nichts.**

- `kDefaultMaterialId` zu ersetzen hieße: jede Vorlagen-Box und die Tutorial-Szene wären
  Wasser, und das echte Fallback (Mesh ohne Material) bliebe trotzdem grau. Gewinn null.
- Form: `EditorDeps/EngineContent/Materials/Water.hasset`, erzeugt von einem neuen Generator
  nach dem `matfn_gen`-Muster (`mat_gen`, UUID-Block **hi=0x400**, lo=1, „anhängen, nie
  umordnen"). Nur so erscheint es ohne Zutun im Content Browser und im Inspector-Picker und
  wird mit dem Spiel ausgeliefert.
- `DefaultAssets.h` bekommt dafür eine **Konstante** `kEngineWaterMaterialId = {0x400, 1}`
  mit Kommentar „lebt als Engine/Materials/Water.hasset", damit Code es per UUID findet.
  **Nicht** zusätzlich in `initDefaultAssets` als `mem://` registrieren: dieselbe UUID zweimal
  (Speicher + Datei) kollidiert, und jede neue `initDefaultAssets`-Zeile bricht
  `tests/test_contentmanager.cpp:925` (`defaultCount == 18`) und `:948` (`materials == 7`).
- Ein `.hasset` darf **keine** `mem://`-Textur referenzieren (im gepackten Spiel nicht
  auflösbar, `HpakWriter.cpp:308–311`). Texturen nur als `Engine/…`-Pfad oder prozedural.
- Varianten (See, Ozean, Sumpf) als Material-Instanzen dieses Elternmaterials.

---

## 5. Parameterliste und Budget

Gezählt einzeln (Flach-/Tieffarbe, Trübung, Tiefenfärbung, 3 Wellenebenen × Richtung/
Geschwindigkeit/Skalierung/Stärke, Fresnel, Spiegelung, Refraktion, Schaumfarbe/-breite/
-stärke, Kaustik-Stärke/-Skalierung/-Tempo, Specular, Rauheit, Transparenz) sind das über 25
Werte, mehr als die **16 Slots**. Deshalb wird gepackt: eine Wellenebene = ein `ParamVec4`,
zusammengehörige Paare = ein `ParamVec2`. Ein-/Aus-Schalter laufen über **Static Switches**,
die keinen Slot kosten (sie erzeugen Shader-Permutationen, ein Ausschalten spart auch
GPU-Zeit).

| # | Name (Param-Knoten) | Typ | Komponenten | Gruppe | braucht neuen Knoten? |
|---|---|---|---|---|---|
| 1 | `ShallowColor` Flachwasserfarbe | Color | rgb | Farbe | — |
| 2 | `DeepColor` Tiefwasserfarbe | Color | rgb | Farbe | — |
| 3 | `Turbidity` Trübung/Absorption | Vec2 | x = Absorption pro Meter, y = Tiefe (m) bis volle Tiefenfarbe | Farbe | SceneDepth |
| 4 | `WaveA` Wellenebene 1 | Vec4 | x = Richtung (Grad), y = Geschwindigkeit (m/s), z = Skalierung (Wellenlänge m), w = Stärke | Wellen | — |
| 5 | `WaveB` Wellenebene 2 | Vec4 | wie WaveA | Wellen | — |
| 6 | `WaveC` Wellenebene 3 (fein) | Vec4 | wie WaveA | Wellen | — |
| 7 | `FresnelPower` | Float | Exponent (Slider 1..10) | Oberfläche | — |
| 8 | `Reflection` Spiegelung/Himmel | Float | 0..1, skaliert den Specular-Pin (→ `heSkyEnv`/SSR in `heLitP`) | Oberfläche | — |
| 9 | `Roughness` | Float | 0..1 | Oberfläche | — |
| 10 | `Specular` | Float | 0..1 (0.5 = F0 0.04; Wasser ≈ 0.25 für F0 0.02) | Oberfläche | — |
| 11 | `Opacity` Transparenz | Float | 0..1, Grund-Deckkraft, Fresnel hebt sie am Rand | Oberfläche | — |
| 12 | `Refraction` | Float | Verzerrungsstärke (Bildschirm-Offset aus der Wellen-Normale) | Refraktion | SceneColor + Screen UV |
| 13 | `FoamColor` Küstenschaum | Color | rgb | Schaum | — |
| 14 | `Foam` | Vec4 | x = Breite (m), y = Stärke, z = Rausch-Skalierung, w = Tempo | Schaum | SceneDepth |
| 15 | `Caustics` Kaustik | Vec4 | x = Stärke, y = Skalierung, z = Tempo, w = Tiefe (m) bis zum Ausblenden | Kaustik | (SceneDepth für w) |
| 16 | — frei — | | | | |

Static Switches (kein Slot): `UseFoam`, `UseCaustics`, `UseRefraction`, `UseNormalMap`
(Normalmap-Textur vs. prozedurale Normale).

Texturen (≤ 4): eine kachelbare Wasser-Normalmap, gelesen von allen drei Wellenebenen
(dedupliziert = 1 Slot), optional eine Schaum-Rauschtextur. Beides gibt es in EngineContent
**noch nicht**; ohne Textur gilt die prozedurale Normale (Abschnitt 6).

UX-Kosten der Packung: der Inspector (`InspectorPanel.cpp:2096–2160`, Abschnitt "Material
Parameters (this entity)", schreibt Pro-Entity-Overrides) zeigt `ParamVec4` als vier
unbeschriftete Zahlen (`Row::dragFloat4`) und **ignoriert Tooltip, Gruppe und Slider-Bereich**
des Param-Knotens. Diese Metadaten kommen heute nur im Material-Editor an
(`MaterialEditorPanel.cpp:1965`). Damit "alle Parameter im Inspector einstellbar" mehr ist
als vier nackte Zahlen, sollte Schritt 2 den Inspector-Abschnitt um Tooltip (Bedeutung der
Komponenten) und Slider-Bereich ergänzen. Das ist eine kleine Editor-Änderung, kein Renderer.

Ohne SceneDepth/SceneColor wirken `Turbidity`, `Refraction`, `Foam` und die Tiefen-
ausblendung der Kaustik nicht so, wie sie sollen. Rückfall ohne neue Knoten: Farbe aus
Fresnel (flach = `ShallowColor` von oben, `DeepColor` am Rand), Deckkraft aus `Opacity` +
Fresnel, Schaum als Rauschmuster über Wellenkämmen statt an der Küste.

### 5a. Stand nach Schritt 2 (gebaut)

`EditorDeps/EngineContent/Materials/Water.hasset`, erzeugt von `mat_gen`
(`src/HE_Tools/src/MatGen/main.cpp`, UUID `kEngineWaterMaterialId = {0x400, 1}` in
`DefaultAssets.h`). Lit, Translucent, Surface, 222 Knoten, keine Textur, keine neuen Knoten.
Geprüft in `tests/test_engine_materials.cpp`. Abweichungen von der Tabelle oben:

| Name | Typ | Komponenten (wie gebaut) | Default |
|---|---|---|---|
| `ShallowColor` / `DeepColor` | Color | rgb | (0.10, 0.42, 0.45) / (0.01, 0.07, 0.12) |
| `Turbidity` | Vec2 | x = Absorption pro m, y = **angenommene** Wassertiefe (m) | (0.35, 3.0) |
| `WaveA/B/C` | Vec4 | x = Richtung (Grad, 0 = +X, 90 = +Z), y = Tempo (m/s), z = Wellenlänge (m), w = **Steilheit** (Höhe/Wellenlänge, die Normale sieht nur die Steigung) | (30, 1.2, 8, 0.25) / (310, 0.8, 3.5, 0.18) / (100, 0.5, 1.2, 0.12) |
| `FresnelPower` | Float 1..10 | Exponent eines eigenen Fresnel aus der **Wellen**normale (der Fresnel-Knoten backt seinen Exponenten und liest `vNormal`) | 5 |
| `Reflection` | Float 0..1 | hebt die Deckkraft mit Fresnel an und skaliert Specular (F0) | 0.8 |
| `Roughness` / `Specular` | Float 0..1 | Specular-Pin = Specular × Reflection | 0.06 / 0.3 |
| `Opacity` | Float 0..1 | Deckkraft senkrecht von oben bei klarem Wasser | 0.55 |
| `Refraction` | Float 0..1 | beugt den Blickstrahl zur Wellennormale: bewegt Tiefentönung und Kaustik mit den Wellen (Szene dahinter noch unverzerrt) | 0.3 |
| `FoamColor` | Color | rgb | (0.92, 0.95, 0.97) |
| `Foam` | Vec4 | x = **Abdeckung** der Wellenkämme 0..1 (statt Breite in m, ohne Szenentiefe gibt es keine Küste), y = Stärke, z = Rauschgröße (m), w = Drift (m/s) | (0.18, 0.8, 1.5, 0.3) |
| `Caustics` | Vec4 | x = Stärke, y = Mustergröße (m), z = Tempo, w = **Kameradistanz** (m), bis zu der das Muster ausgeblendet ist (statt Tiefe; verhindert Moiré in der Ferne) | (0.35, 2.5, 0.25, 25) |

- **Keine Static Switches.** Der Inspector zeigt nur `graphParamNames`. Schalter wären dort
  unsichtbar und nur per Material-Instanz änderbar. Aus = Stärke 0.
- Wellen: gerichteter Sinus pro Ebene, Phase mit FBM verzerrt (bricht die Kämme), Normale
  analytisch `N = normalize(vNormal − Σ steep·cos(phase)·d)`. Setzt eine ungefähr
  waagrechte Fläche voraus. Kein WPO, die Engine-Plane ist ein Quad.
- Wasserkörper: `T = exp(−Absorption · Tiefe / (N·V + 0.05))`, Farbe `mix(Shallow, Deep, 1−T)`,
  Deckkraft `Opacity → 1` mit `1−T`, dann mit `Reflection·Fresnel`, dann mit Schaum.
- Die Datei trägt den **gebackenen** Shader (der Packer liefert ihn verbatim aus).
  `mat_gen` speichert, lädt über einen frischen ContentManager (Editor-Ladepfad) und
  speichert noch einmal.
- **Inspector-Reihenfolge ist compilerabhängig** (MSVC: Foam, WaveC, WaveB, WaveA, …),
  siehe Thema-152-Warnung zum Codegen. **Idee, nicht umgesetzt (Chefchen):** der
  Inspector könnte Tooltip und Sliderbereich der Slots zeigen (`graphParamTooltips`,
  `graphParamMinMax` sind im Asset gefüllt) und Vec4-Komponenten beschriften.
  Seit Schritt 4 haben die Wasser-Zeilen Tooltips aus der Hilfe-Tabelle (§9.3), die
  Asset-Tooltips liest der Inspector weiterhin nicht.
- `matGraphApproxSurface` kann die BaseColor des Wassers nicht falten (sie hängt an
  `ViewDir`/`Time`), GI-Treffer auf Wasser bekommen dann Weiß. Laut Code
  (`MaterialGraph.h:318–322`), nicht gemessen. Kleiner Nebenbefund.

---

## 6. Fehlende Graph-Knoten (benannt, nicht gebaut)

Nach Kosten sortiert. "Nur Codegen" heißt: `MaterialGraph.cpp` + Registry + Doku-Stellen,
kein Renderer. "Renderer" heißt: neue Bindung in allen fünf Backends.

| Knoten | Wofür | Kosten |
|---|---|---|
| **Screen UV** (`gl_FragCoord.xy / heLight.giParams.xy`) | normierte Bildschirm-UV für SceneColor/SceneDepth; `Screen Position` liefert nur rohe Pixel. Y-Konvention pro Backend beachten | nur Codegen |
| **Normal from Height** (oder `DDX`/`DDY`) | Wellen-Normale aus prozeduraler Höhe (FBM/Sinus) ohne Textur. Heute nur per Hand: Höhe an p, p+dx, p+dz auswerten → `Combine3((h-hx)/e, 1, (h-hz)/e)` → `Normalize3` → Normal-Pin, kostet pro Ebene die dreifache Rauschrechnung | nur Codegen |
| **Wave / Gerstner** (optional) | Summe gerichteter Wellen mit analytischer Normale aus (Richtung, Tempo, Länge, Stärke). Spart den Graph-Aufbau aus Sinus/Dot/Combine für jede Ebene | nur Codegen |
| **Cosine / Rotate 2D** (optional) | Richtung in Grad → Vektor. Geht heute als `Sine(x + π/2)` | nur Codegen |
| **SceneDepth** + **Depth Fade** | lineare Tiefe der opaken Szene hinter dem Pixel; `Depth Fade` = Szenentiefe − Pixeltiefe = Wassertiefe. Grundlage für Tiefenfärbung, Trübung, Küstenschaum, Kaustik-Ausblendung | **Renderer**: Tiefenkopie vor dem Translucent-Pass + neue Bindung in der Material-Preamble |
| **SceneColor** (Surface-Gegenstück zu `Backdrop`) | opake Szenenfarbe hinter dem Pixel, an Screen UV + Offset gelesen → Refraktion | **Renderer**: Farbkopie vor dem Translucent-Pass + neue Bindung |

Kaustik ist auf einem Oberflächen-Material nur als Muster **auf der Wasserfläche** machbar
(zwei gegenläufig gepannte FBM, `1 - abs`, `Power`). Echte Kaustik **auf dem Grund**
bräuchte eine Projektion auf die opake Szene (Decal/Licht-Cookie) und gehört zum
ausgeschlossenen Unterwasser-Teil.

---

## 7. Folgen für die nächsten Schritte

- **Translucent-Graph läuft heute auf allen fünf Backends** (Abschnitt 3). Ein Wasser ohne
  SceneColor/SceneDepth braucht für die Grundfunktion keinen neuen Pass.
- **Vor dem GL-Screenshot (Schritt 3/4) muss Fehler 1 aus Abschnitt 3 behoben sein:** im
  GL-Translucent-Zweig `HeLighting` hochladen (Dedupe-Flag beachten) und Units 13–17 binden,
  wie im opaken Zweig. Kleine Renderer-Änderung, eigener Schritt oder Teil von Schritt 3.
  Sonst animiert das Wasser auf GL forward nicht und spiegelt keinen Himmel, wenn es das
  einzige Graph-Material im Bild ist. Bis dahin GL-Bildläufe mit Deferred-Pfad oder mit einem
  zweiten, opaken Graph-Material in der Szene (verdeckt den Fehler nur!).
- **Schritt 2 ohne Renderer machbar:** `mat_gen` + `Water.hasset` + Konstante in
  `DefaultAssets.h`; Graph mit Wellen aus `WorldPos.xz`, prozeduraler oder Normalmap-Normale,
  Fresnel-Farbmischung, lit + Translucent, Parametern nach Abschnitt 5. Dazu Inspector-
  Abschnitt um Tooltip/Slider ergänzen (Editor). Neue Codegen-Knoten (Screen UV, Normal
  from Height) gehen ohne Renderer; jeder neue Knoten braucht die Registry-Stellen
  (Display-Name, Doku, Tests).
- **Renderer-Arbeit, eigener Schritt/Thema:** SceneDepth + SceneColor (Kopien vor dem
  Translucent-Pass, neue Bindungen in fünf Backends). Erst danach wirken Tiefenfärbung,
  Trübung, Küstenschaum und Refraktion echt; bis dahin der Fresnel-Rückfall aus Abschnitt 5.
- Eine Wasser-Normalmap als `Engine/`-Textur fehlt; ohne sie prozedurale Normale.
- „Sichtbar animiert auf einer Plane" geht über Normalen/UV; WPO braucht ein unterteiltes
  Mesh (Engine-Plane ist ein Quad).

---

## 8. Backend-Parität (Schritt 3, gebaut und gemessen)

Was die Renderer bauen: D3D11, D3D12, Vulkan und GL 4.3 nehmen zuerst `fragmentClustered`
und fallen auf `fragment` zurück; Metal und GL 4.1 bauen `fragment`. Ein Export backt pro
Backend `bakeMaterialShaderVariant`. Das Wasser hat keine Textur, bringt also keinen eigenen
Sampler mit; geprüft wird, ob die Lighting-Preamble, die es mitzieht, in die Wände jedes
Backends passt.

### 8.1 In he_tests (`tests/test_engine_materials.cpp`, läuft in CI auf allen drei OS)

Jeder Prüfer hat eine eigene Negativkontrolle (ein Eingang, den er ablehnen muss).

| Backend | Richter | Stand |
|---|---|---|
| GL 4.1 / ES 3.0 / GL 4.3 (geclustert) | glslang im GL-Modus parst und **linkt** Vertex + Fragment (forward, G-Buffer, gebacken); Reflexion des gelinkten Programms | grün |
| Metal | Sampler nur im Material-Fenster 0..4 oder auf `kMetalPreambleSamplerSlots`, ≤ 16; Textur-Slots ≤ 30. **Nur Text**, kein Metal-Compiler (§8.2) | grün |
| D3D11/D3D12 (Windows) | FXC ps_5_0 plain/geclustert/gebacken + vs_5_0; Bytecode-Reflexion: s ≤ 15, ≤ 16 Sampler, b ≤ 13; Abdeckung durch `D3D12MaterialRootSignature.h`; echtes **Translucent-PSO auf D3D12-WARP**; D3D11-WARP `CreateVertexShader`/`CreatePixelShader`/`CreateInputLayout` | grün |
| HLSL-Text (alle OS) | SM-5.0-Wände, keine zwei lebenden Sampler auf einem s-Register (X4500) | grün |
| Vulkan | SPIRV-Cross: aktive Deskriptoren von Vertex, plain und geclustert inkl. **Stage** gegen `VulkanMaterialLayout.h`; geclustert fügt genau 24..26 hinzu; kein Binding 14 (Thema 143); ≤ 16 Sampler | grün |
| Export | `bakeMaterialShaderVariant` für alle fünf `RendererBackend`, regeneriert und aus der Datei: ok, keine Warnung; GL-4.10- und 4.30-Paar linken; SPIR-V entpackbar; D3D-HLSL durch FXC | grün |

**HeParams-Layout**, eine Abmachung, vier Sichten: jeder Renderer kopiert
`min(shaderParamData.size(), 64)` Floats in einen 256-Byte-Block. Geprüft: SPIR-V set 0
binding 3, 256 B, `v[16]` bei Offset 0, Stride 16; gelinktes GL-Programm: Block `HeParams`
256 B, Bindung per Name (kein `binding =`), ES hält ihn `highp` trotz `precision mediump float`;
FXC-Reflexion: cbuffer `HeParams` auf b3 = Root-CBV `kRootParamsCB`, 256 B, `float4[16]`;
MSL: `float4 v[16]`, `HeParams` auf `buffer(2)`, `HeLighting` auf `buffer(1)`. Dazu
`HeLighting` = `sizeof(MaterialShaderLibrary::Lighting)` in SPIR-V, GL und FXC. CPU: 15 Knöpfe
= 60 Floats ≤ 64, jeder Default in den Komponenten seines Typs. Jede Lesestelle
`heParams.v[i]` nutzt den Swizzle ihres Typs (Float `.x`, Vec2 `.xy`, Color `.xyz`, Vec4 ohne),
alle 15 Slots werden gelesen, keiner ≥ 16. Slot-Indizes werden nie per Position angenommen
(Reihenfolge ist compilerabhängig, §5a).

### 8.2 Offline mit den SDK-Werkzeugen

`python scripts/water_shader_offline_check.py` lässt he_tests jede Variante dumpen
(`HE_DUMP_WATER_SHADERS`) und schickt sie durch die Werkzeuge, jedes nach einer
Negativkontrolle. NN-WS03, 2026-10-05: **13 bestanden, 0 fehlgeschlagen, 1 übersprungen.**

- `glslangValidator -l` (GL-Modus, beide Stufen gelinkt): GL 4.1 forward, GL 4.1 G-Buffer,
  ES 3.0, GL 4.3 geclustert.
- `spirv-val --target-env vulkan1.2` (das Ziel von `he::shaderc`): Vertex, Fragment, geclustert.
- `fxc /T vs_5_0|ps_5_0 /E main`: Vertex, Fragment, geclustert. Warnung X3570 aus
  `heLocalShadowFactor` (PCF-Schleife der gemeinsamen Preamble, jedes beleuchtete
  Graph-Material); FXC rollt ab und baut.
- MoltenVK, erste Hälfte: `spirv-cross --msl --msl-version 20100` über das Vulkan-SPIR-V
  (Vertex, Fragment, geclustert); 11 Fragment-Sampler, unter Metals 16.
- FXC-Warnungen X3556 (Integer-Modulo/-Division) und X3570 stammen alle aus der Preamble.
- **`xcrun metal`: grün in CI** (Lauf 37347421486, macOS-Job, Apple metal 32023.883, Commit
  62e6a410). Negativkontrolle abgelehnt, dann `metal -c` ok für Vertex, forward, geclustert,
  G-Buffer und das in der Datei gebackene Fragment (5/5). Der G-Buffer meldet eine Warnung
  (Art im Log abgeschnitten, das Skript zeigt Warnungszeilen seither ganz). Die regenerierten
  Varianten kamen dort aus clang, das gebackene Fragment aus der MSVC-Datei; beide bauen.
  Das MoltenVK-MSL lief dort nicht durch `metal -c`, weil `spirv-cross` auf dem Runner fehlt.
  Auf einem Mac mit Vulkan SDK übersetzt `python3 scripts/water_shader_offline_check.py
  --require-msl` beides. Der macOS-Job in `ci.yml` ruft das nach dem Abruf der
  Metal-Toolchain auf, auf `main`, in PRs und per `gh workflow run CI --ref <zweig>`.
- Im selben Lauf liefen die neuen he_tests-Fälle zum ersten Mal unter clang und gcc: macOS
  237/237, Linux (GCC 13.3) grün, Windows 238/238, lavapipe-Bildtests grün.

### 8.3 Auf echter GPU (Rauchtest, keine Abnahme)

Zeuge `HE_DUMP_WATERTEST=floor` (Editor, §8.4), RTX 4070, Editor-Deploy des Zweigs,
`HE_DUMP_SKYTEST` mit CAMY 4, CAMZ 6, PITCH −25, TOD 0.4, 16 Settle-Frames, 1280×720,
frisches `APPDATA` pro Lauf (kalter GL-Programm-Cache). Gemessen im Wasserband (untere 60 %),
mittlere absolute Differenz in 0..255:

| Backend | Helligkeit | t = 1.0 zweimal | t = 1.0 gegen 3.5 | gegen GL forward |
|---|---|---|---|---|
| GL forward | 143.33 | 0 | 22.4 | — |
| GL deferred | 143.35 | 0 | 22.4 | 0.05 |
| D3D11 | 142.92 | 0 | 23.1 | 1.8 |
| D3D12 | 142.37 | 0 | 23.6 | 4.1 |
| Vulkan | 142.39 | 0 | 23.6 | 4.1 |

Alle fünf Läufe (vier Backends, GL zweimal) animieren, sind reproduzierbar und zeigen dasselbe
Bild; Metal fehlt in dieser Tabelle (Metal und GL auf dem Mac: §9.1). D3D12 und Vulkan zeichnen die
Kaustiklinien sichtbar etwas weicher als GL/D3D11 (Ursache nicht untersucht). D3D12-Debug-Layer:
nur die bekannte `ClearRenderTargetView`-Warnung. Vulkan-Validierung: nur
`vkCmdUpdateBuffer`/Barriere im Render-Pass (Thema 144, vorbestehend). **Nicht** belegt: eine
zweite GPU (AMD/Intel), das exportierte Spiel, D3D11 mit Debug-Layer (`HE_GPU_DEBUG` wirkt dort
nicht), Metal überhaupt.

### 8.4 GL-Fehler 1 behoben (`OpenGLRenderer.cpp`, Translucent-Zweig)

Der Zweig lädt `HeLighting` jetzt selbst hoch, wenn es in diesem Frame noch niemand getan hat
(dieselbe Einmal-Regel wie der opake), und bindet die Units wie der opake: 13 Weightmap (beim
Einsammeln aufgelöst), 14 Himmels-Cube, 15 AO, 16/17 DDGI. Gleicher Zeuge, gleiche Einstellungen,
nur das Wasser als Graph-Material im Bild:

| GL | Helligkeit | t = 1.0 gegen 3.5 |
|---|---|---|
| forward, **vorher** | 0.41 (schwarz) | 0 (steht still) |
| forward, nachher | 143.33 | 22.4 |
| deferred, vorher = nachher | 143.35 (bytegleich) | 22.4 |

Der Zeuge (`HE_DUMP_WATERTEST`, `=floor` mit grauem Boden 1.5 m darunter) lädt das ausgelieferte
`Engine/Materials/Water.hasset` auf die Engine-Plane (40 m, y = 0) und ist für den Screenshot in
Schritt 4 gedacht.

### 8.5 Offen und Nebenbefunde

- **Metal:** Das Kompilat ist belegt (§8.2, CI), das Bild im Editor seit Schritt 4 auch
  (§9.1). Das exportierte Spiel auf Metal ist weiter ungeprüft. MoltenVK: nur übersetzt
  (`spirv-cross --msl`), nicht kompiliert.
- **Vulkan 1.0:** `he::shaderc` erzeugt SPIR-V 1.5 (Vulkan 1.2), `VulkanRenderer` fällt bei einem
  Loader unter 1.2 auf eine 1.0-Instanz zurück, und `spirv-val --target-env vulkan1.0` lehnt
  dieselben Module ab. Das betrifft jeden Shader aus `he::shaderc`, nicht das Wasser. Ob ein
  echtes 1.0-System das trifft, ist ungeprüft.
- §3 Fehler 3 (GL/Metal: Graph-Texturen bleiben auf Units 1–4) unverändert, das Wasser hat keine
  Textur.
- Debug-`test_material_graph` braucht auf NN-WS03 643 s (vorbestehend, WARP-Fälle).

---

## 9. Bilder auf Metal und GL, Tooltips, Handbuch (Schritt 4)

### 9.1 Headless-Aufnahmen auf dem Mac (M5, macOS 27, Release-Editor des Zweigs)

Rezept wie §8.3, damit die Zahlen zur Windows-Tabelle passen:

```
HE_SKY_TIME=1.0 HE_CONFIG_DIR=<frisch pro Lauf> HE_SHOT_TIMEOUT=400 \
  python3 scripts/he_shot.py out.png WATERTEST=floor TOD=0.4 CAMY=4 CAMZ=6 PITCH=-25 \
  RHI=Metal|OpenGL RENDERPATH=0|1 AA=0 [WATERPARAMS="Name=v0,v1,…;Name2=…"]
```

1280×720, `he_shot` erzwingt `SKYTEST=1` (Himmel aktiv). Jeder Lauf meldet
`dump counters — draws=2 tris=14 visible=2/2` (forward; deferred `draws=3`) und
`0 error(s)`. Orakel: mittlere absolute Differenz in 0..255, Wasserband = untere 60 %.

| Vergleich | Metal | OpenGL 4.1 |
|---|---|---|
| t = 1.0 zweimal (Rauschboden) | md5-gleich | md5-gleich |
| t = 1.0 gegen 3.5, forward (Animation) | 25.58 | 25.58 |
| t = 1.0 gegen 3.5, deferred | 25.58 | — |
| forward gegen deferred, t = 1.0 | 0.02 | 0.09 |
| Metal gegen GL, forward t = 1.0 / t = 3.5 | 0.07 / 0.06 | |
| Metal gegen GL, deferred t = 1.0 | 0.05 | |

Damit ist das erste Metal-Bild des Wassers belegt, und Metal und GL zeichnen auf dem Mac
dasselbe Bild. Sichtbar: drei Wellenzüge, Himmelsspiegelung, die zum Horizont hin zunimmt
(Luma der Bildstreifen von 94 vorn auf 130 am Horizont), heller Flach- und dunkler Tiefton,
Kaustiklinien und Schaumflecken auf den Kämmen.

### 9.2 Was jeder Knopf bewirkt (Metal, t = 1.0, je gegen die Aufnahme ohne Override)

Neuer Schalter am Zeugen: `HE_DUMP_WATERPARAMS` setzt `MaterialComponent::paramOverrides`
der Wasser-Plane, also genau das, was der Inspector unter „Material Parameters (this
entity)" schreibt. Nicht genannte Komponenten behalten den Material-Default. Kontrollen:
ohne Schalter und mit einem unbekannten Namen (`Nope=1`, nur eine Warnung im Log) ist das
Bild md5-gleich zur Aufnahme vor dem Umbau.

| Override | Wasserband | Befund |
|---|---|---|
| `DeepColor=1,0,0` | 68.97 | Tiefentönung: das Wasser wird rot, zum Horizont stärker (R 193 vorn, 217 am Horizont) |
| `Turbidity=0,3` (keine Absorption) | 41.00 | klares Wasser, deutlich heller (Luma vorn 94 → 137) |
| alle drei Wellen `w = 0` | 14.57 | Wellen weg (PNG 579 → 380 KB) |
| `Reflection=0` | 2.69 | schwach |
| `FresnelPower=1` | 0.44 | kaum sichtbar |
| `Turbidity=0,3` + `FresnelPower=1` / `=10` | 7.38 / 1.03 | in klarem Wasser wirkt Fresnel, monoton |
| `Turbidity=0,3` + `Reflection=0` | 2.57 | schwach |
| GL: `DeepColor` / Wellen aus, gegen Metal | 0.07 / 0.06 | Override-Pfad auf GL gleich |

**Befund zur Abstimmung (nicht behoben):** Mit der Standard-Trübung (0.35/m, 3 m) ist das
Wasser in dieser Kamera fast deckend, die Tiefentönung überdeckt dann den Fresnel-Lift der
Deckkraft. Die cremefarbenen Himmelsstreifen bleiben auch bei `Reflection=0`; sie kommen
vermutlich aus dem Fresnel in `heLitP`, der bei streifendem Blick unabhängig vom F0 gegen 1
geht. `FresnelPower` und `Reflection` sind damit im Default-Wasser schwache Knöpfe. Wer das
ändern will, stimmt `mat_gen` ab (z. B. Reflection auch auf die Himmelsfarbe statt nur auf F0)
und erzeugt `Water.hasset` neu; die Tests in `test_engine_materials.cpp` prüfen Mengen und
Defaults und müssen dann mitgezogen werden.

### 9.3 Tooltips und Handbuch

- Die Inspector-Zeilen der Wasser-Parameter tragen den **Parameternamen** als Label
  (`Row::colorEdit3/dragFloat2/dragFloat4/dragFloat`), die Hilfe sucht also unter dem
  Komponenten-Scope `Material/<Name>`. Neu in `src/HE_Editor/EditorHelp.cpp`: 13 Einträge
  `Material/ShallowColor` … `Material/Caustics`, Topic `materials#parameters` (seit Schritt 5
  `materials#water`, §10.2), Text aus den
  Asset-Tooltips von `mat_gen`, ausformuliert. `Roughness` und `Opacity` teilen sich die
  Einträge mit dem Surface-Block derselben Komponente (ein Schlüssel kann nur einmal
  existieren); beide haben einen Satz zum Wasser dazubekommen.
- `editor_help_audit` sieht diese Zeilen nicht (Label = Daten). Deshalb prüft
  `tests/test_engine_materials.cpp` („every knob has a tooltip in the Details panel") jeden
  Parameternamen des ausgelieferten Assets unter dem Scope `Material` gegen die Tabelle,
  mit Negativkontrolle `WaveD`. Ein in `mat_gen` umbenannter oder neuer Knopf wird dort rot.
- **Handbuch:** Die Einträge landen von selbst in der generierten Editor-Referenz des
  In-Engine-Handbuchs (Komponentenseite, Gruppe Material), F1 auf einer Zeile öffnet dort
  den eigenen Abschnitt, „Mehr dazu" zeigt seit Schritt 5 auf `materials#water`.
- **Website-Handbuch:** seit Schritt 5 erledigt, siehe §10.2.

### 9.4 Nicht belegt

- „Sichtbar animiert im Editor" ist über die Headless-Aufnahmen belegt (gleicher Renderer,
  gleiche Welt), nicht durch Bedienen des Editors. Die Tooltips sind über den Lookup-Test
  belegt, nicht durch ein Bild des schwebenden Tooltips.
- Das exportierte Spiel auf Metal/GL.

---

## 10. Verifikation (Schritt 5)

### 10.1 Build und Tests (Mac, M5, macOS 27.0.1, Apple clang 21, 2026-10-06)

`out/build/macos-release` im Worktree: Release, Unix Makefiles, `HE_ENABLE_SHADERC=ON`,
`HE_VALIDATE_SHADERS=ON`, `HE_BUILD_TESTS=ON`. Alles im Vordergrund, Ergebnis aus dem Log
und dem Rückgabewert gelesen, nicht aus einer Zusammenfassung.

- `cmake --build . -j8`: rc 0, alle 60 Ziele (Engine, Editor, Werkzeuge inkl. `mat_gen`,
  `HeValidateShaders`, `he_tests`). **Inkrementell**: die Engine-Ziele stammten aus dem
  Build von Schritt 4; geprüft, dass `he_tests` und `HorizonEditor` jünger sind als jede
  Datei, die der Zweig gegen `main` ändert. Nach den Änderungen dieses Schritts noch einmal:
  rc 0, neu übersetzt nur `EditorHelp.cpp` (Editor + he_tests) und `test_engine_materials.cpp`.
  Einzige Warnungen: `ld: ignoring duplicate libraries` (vorbestehend).
- `ctest` in drei Stücken, `-j4`, eigenes `TMPDIR`: **235 bestanden, 0 fehlgeschlagen,
  2 übersprungen** von 237. Die zwei sind `runtime_size_app_basic|advanced`, sie messen ein
  exportiertes `out/deploy/AppBasic|AppAdvanced`, das es im Worktree nicht gibt (nicht
  Wasser-bezogen). `test_material_graph` 200 s, `test_engine_materials` 15 s.
- `scripts/water_shader_offline_check.py` lokal: 10 bestanden, 0 fehlgeschlagen,
  2 übersprungen. GL/ES `glslangValidator -l` (4 Paare), `spirv-val vulkan1.2` (3),
  `spirv-cross --msl` (MoltenVK-Übersetzung, 3), jeweils nach Negativkontrolle. Übersprungen:
  `fxc` (kein Windows SDK) und `xcrun metal` (Metal-Toolchain auf diesem Mac nicht geladen).
  Beides ist in CI belegt (§8.2, Lauf 37347421486), dieser Schritt ändert keinen Shader.
- **Volle CI-Matrix auf 05cb0a25** (Lauf 37393126985, per `gh workflow run CI --ref`):
  alle vier Jobs grün. macOS 235 bestanden + 2 übersprungen von 237, dazu `xcrun metal`
  5/5 auf dem Wasser-MSL (Vertex, forward, geclustert, G-Buffer, gebacken); Linux 235 + 2
  von 237; Windows 236 + 2 von 238 (FXC/WARP-Fälle in he_tests); lavapipe-Bildtests 1/1 und
  4/4. Die übersprungenen sind überall `runtime_size_app_basic|advanced`.

### 10.2 Website-Handbuch

- Abschnitt `#water` „Engine Water" in `HorizonEngineDocs/materials.html` mit Bild
  `water_engine.png` (Metal, Rezept §9.1, t = 1.0): Verwendung, alle 15 Parameter mit
  Bedeutung und Default, Grenzen. Repo HC-Website, Zweig
  `claude/eigener-wasser-shader-als-engine-default-material` (von `origin/main`, Commit
  b9809f8), gepusht, **nicht gemergt und nicht deployt**: das Veröffentlichen braucht die
  Bestätigung des Menschen. Der lokale `main`-Checkout der Website (fremde Änderungen,
  ahead 2 / behind 2) ist unberührt.
- `EditorDeps/Docs/he-docs.json`: **nur die Seite `materials` ersetzt**, plus
  `img/water_engine.jpg`. Ein voller Neulauf von `build_docs_bundle.py` hätte 18 fremde
  Abschnitte anderer Themen mitgebracht, das eingecheckte Bündel war schon vorher
  gegen den Website-Stand veraltet. Die anderen sechs Abschnitte der Seite sind
  unverändert, die übrigen Bilder byte-gleich (Pillow in einer Wegwerf-venv).
  `build_docs_bundle.py --check` meldet das Bündel deshalb weiter als veraltet.
- Die 13 Wasser-Einträge in `EditorHelp.cpp` zeigen jetzt auf `materials#water` statt
  `materials#parameters`. `test_engine_materials` pinnt das, und `test_editor_help`
  („every topic it points at exists in the manual") prüft, dass der Abschnitt im
  ausgelieferten Bündel existiert.

### 10.3 Was nur syntaktisch geprüft ist, was auf echter Hardware offen bleibt

| Backend | Kompilat | Bild |
|---|---|---|
| Metal | `xcrun metal` 5/5 (CI) | Editor headless, M5 (§9.1) |
| OpenGL 4.1 | glslang gelinkt (he_tests, lokal) | Editor headless, M5 (§9.1); GL 4.3 + ES nur Compiler |
| D3D11 | FXC + WARP-Shader-Objekte (he_tests, Windows-CI) | ein Rauchtest im Editor, RTX 4070 (§8.3) |
| D3D12 | FXC + WARP-Translucent-PSO (Windows-CI) | ein Rauchtest im Editor, RTX 4070 (§8.3) |
| Vulkan | spirv-val 1.2, SPIR-V-Reflexion, lavapipe-Job grün | ein Rauchtest im Editor, RTX 4070 (§8.3) |
| MoltenVK | nur `spirv-cross --msl`, nie `metal -c` | nie |

Offen auf echter Hardware: AMD- und Intel-GPUs (D3D11/D3D12/Vulkan), D3D11 mit Debug-Layer
(`HE_GPU_DEBUG` wirkt dort nicht), Vulkan auf einem 1.0-Loader (§8.5), das **exportierte
Spiel** auf allen fünf Backends, und das Bedienen im laufenden Editor (alles Bildliche kommt
aus dem Headless-Zeugen). Für den GL-Fix aus §8.4 gibt es keinen CI-Test (kein GL-Kontext in
CI), nur das lokale A/B. Offene Abstimmung: `FresnelPower`/`Reflection` wirken bei der
Default-Trübung kaum (§9.2).

---

## 11. Realistischeres Aussehen (Schritt 7)

Anlass: das Urteil „ich will ein realistischeres Aussehen, das sieht sehr comichaft aus"
zum Wasser aus Schritt 4. Es gibt weiter keinen neuen Graph-Knoten, keine neue Textur, keinen
Renderer-Eingriff und keinen neuen Knopf: dieselben 15 Namen, dieselben Typen, neue Defaults und
ein umgebauter Graph in `mat_gen` (`src/HE_Tools/src/MatGen/main.cpp`).

### 11.1 Was am alten Bild comichaft war

Bild: `~/.claude/hive/artifacts/thema152-schritt7/old_*.png` gegen `new_*.png` (gleiche
Kamera, Zeit, Backend; Zeuge wie §9.1).

1. **Ein Gitter gleicher Flecken.** Drei parallele Sinus mit Steilheit 0,25/0,18/0,12 (Summe 0,55,
   echtes Meer liegt bei 0,05 bis 0,3) kippen die Normale so weit, dass ganze Felder auf den hellen
   Horizontstreifen des Himmels-Cubes springen. Das Ergebnis sind cremefarbene, weich begrenzte
   Flecken in Reihen.
2. **Kaustik-Netzlinien auf der Oberfläche** (Stärke 0,35, `×6`, `pow 5`, additiv). Eine helle
   Linie auf dem Wasser ist das, was ein Comic zeichnet; echte Kaustik liegt auf dem Grund.
3. **Weiße Schaumblobs** mit Deckkraft 1 auf jedem Kamm (Abdeckung 0,18, Stärke 0,8).
4. **Gesättigter Türkiskörper** (0,10/0,42/0,45): Der Himmel trägt in `heLitP` Diffus (`ambDiff ×
   0,35`) und Reflexion, die Körperfarbe stand dagegen und machte aus dem Cremehimmel ein
   Graublau. Echtes Wasser ist überwiegend Spiegel, der Körper dunkel.
5. **Glatte, scharfkantige Reflexion** (Rauheit 0,06 ohne Distanzanteil) und keinerlei kurze,
   unregelmäßige Wellen: nichts zerlegte den Sonnenstreifen in Glitzer.

### 11.2 Was geändert ist

| Ursache | Änderung im Graph |
|---|---|
| Gitter (1) | Jeder Wellenknopf treibt zwei Züge: sich selbst und einen **Begleitzug** (um 38°/−33°/47° gedreht, Wellenlänge ×0,58/0,64/0,71, Steilheit ×0,55/0,55/0,60, Tempo ∝ √Wellenlänge wie Tiefwasser-Dispersion). Der Begleitzug fährt kein eigenes FBM, sondern **erbt das Warp-Feld** des Primärzugs (×0,9), sonst sind seine Kämme geradlinig und kreuzen die gebogenen als Rautengitter (gemessen: ohne Warp-Teilung bleibt das Gitter sichtbar, `WaveC` aus = weg). Zwei Schichten **Chop** (Steigung eines FBM-Höhenfeldes per endlicher Differenz, 2,6 m und 0,9 m, treibt mit Zug B bzw. C): ohne sie bleibt eine Summe von Sinus ein glattes Interferenzmuster. Ein **Böenfeld** (FBM, 22 m) tauscht das Gewicht zwischen Zug und Begleitzug und variiert die Helligkeit des Körpers um ±5 %. Feine Züge und Chop **blenden mit der Entfernung aus** (Sinus unter einem Pixel flimmern, es gibt keine Mips). |
| Netzlinien (2) | Kaustik **hellt den Körper multiplikativ auf** (`body × (1 + 3·web·Stärke·Transmission·Nähe)`) statt eine weiße Linie zu addieren; Netz breiter und weicher (`×3,5`, `pow 3`). Sie hängt an der Transmission des Wassers, ist also in klarem, flachem Wasser sichtbar (Turbidity 0,1, siehe `new_clear_caustics_user_params.png`) und in tiefem, trübem nicht. Default-Stärke 0,35 → 0,2. |
| Blobs (3) | Foam-Kamm aus der **Summe von B, seinem Begleitzug und C** statt aus allen sechs (die Summe aller sechs hat Maxima als runde Flecken, drei Sinus ergeben kammförmige Streifen). Zwei Rauschmaßstäbe (grob entscheidet wo, fein franst aus), weichere Kante, Schaumfarbe durch das Rauschen moduliert (nicht flach weiß), Deckkraft höchstens 0,85. Schwelle `0,97 − 0,9·Abdeckung`. Default-Abdeckung 0,18 → 0,06, Stärke 0,8 → 0,5, Rauschgröße 1,5 → 0,7 m. |
| Körper (4) | Farben dunkler und entsättigt (Tabelle), Absorption 0,35 → 0,45 /m bei 4 m angenommener Tiefe. Auf den **Kämmen der Dünung** (nur Zug A und sein Begleiter, nicht alle sechs: die Summe ergab ein Fleckengitter) kommt ein Zehntel der Flachfarbe zurück (Licht scheint durch dünne Kämme). Deckkraft 0,55 → 0,5. |
| Reflexion (5) | Rauheit 0,06 → 0,08, dazu **+0,25 mit der Entfernung** (10 m bis 160 m): was ein Pixel nicht mehr auflöst, mittelt eine rauere Fläche. Kein Horizontstreifen aus Einzelpixel-Glitzern. |

Neue Defaults (alle 15 Knöpfe, Test-Tabelle `kWaterKnobs` ist nachgezogen):

| Knopf | alt | neu |
|---|---|---|
| `ShallowColor` | 0.10, 0.42, 0.45 | 0.045, 0.20, 0.22 |
| `DeepColor` | 0.01, 0.07, 0.12 | 0.004, 0.028, 0.052 |
| `Turbidity` | 0.35, 3.0 | 0.45, 4.0 |
| `WaveA` | 30, 1.2, 8, 0.25 | 20, 1.0, 14, 0.07 |
| `WaveB` | 310, 0.8, 3.5, 0.18 | 335, 0.8, 5, 0.10 |
| `WaveC` | 100, 0.5, 1.2, 0.12 | 70, 0.5, 1.6, 0.09 |
| `Roughness` | 0.06 | 0.08 |
| `Opacity` | 0.55 | 0.5 |
| `FoamColor` | 0.92, 0.95, 0.97 | 0.82, 0.88, 0.90 |
| `Foam` | 0.18, 0.8, 1.5, 0.3 | 0.06, 0.5, 0.7, 0.3 |
| `Caustics` | 0.35, 2.5, 0.25, 25 | 0.2, 2.0, 0.2, 18 |
| `FresnelPower`, `Reflection`, `Specular`, `Refraction` | unverändert | unverändert |

Bedeutungsänderungen, die in Tooltips (`mat_gen`, `EditorHelp.cpp`) und im Handbuch
(`he-docs.json`, Abschnitt Engine Water) nachgezogen sind: Die drei Wellenknöpfe treiben je zwei
Züge, und ihre Steilheiten zusammen setzen auch die Stärke des Chops (alle drei 0 = glatt). Kaustik
hellt den Körper auf. Foam sitzt auf den Kämmen der kürzeren Züge. Rauheit wächst mit der Entfernung.

### 11.3 Messung (Mac M5, Release-Editor des Zweigs, Rezept §9.1, 1280×720)

| Vergleich | Metal | OpenGL 4.1 |
|---|---|---|
| t = 1.0 zweimal (Rauschboden) | md5-gleich | md5-gleich |
| t = 1.0 gegen 3.5, Wasserband | 10,60 | 10,60 |
| forward gegen deferred, t = 1.0 | 0,01 | 0,07 |
| Metal gegen GL, forward t = 1.0 / 3.5 | 0,06 / 0,05 | |
| Metal gegen GL, deferred | 0,04 | |
| Metal gegen GL, Blick zur Sonne (TOD 0,33, YAW 90) | 0,10 (Wasserband) | |

Die Animation ist ruhiger als vorher (vorher 25,6), weil die Wellen flacher sind; sie ist weiter
klar sichtbar (95 % der Pixel ändern sich). Die Metal/GL-Abweichung ist **kleiner** als vorher
(0,07). Alle FBM-Knoten des Wassers laufen jetzt auf dem Integer-Hash (`Fbm`-Knoten,
`p[0] = 1`, `heFbmI`), der laut Codegen auf allen Backends bitgleich dasselbe Feld liefert; der
Float-Hash des einfachen `Fbm` rundet bei großen Weltkoordinaten je GPU anders. Dass der Gewinn
von dieser Umstellung kommt, ist eine Vermutung, nicht gemessen (keine A/B mit dem Float-Hash).

Andere Einstellungen als die Rezept-Kamera (TOD 0,27/0,33/0,4/0,5, Kamera 2,5 m bis 12 m, Blick
5° bis 60° nach unten, Sonne im Bild und nicht): Das Gitter ist in keiner der aufgenommenen
Einstellungen mehr sichtbar, der Sonnenglitzer erscheint, wo die Wellen zur Sonne kippen
(`new_sun.png`), die Himmelsstreifen sind schmal statt flächig. Das ist Augenschein an etwa
zwanzig Aufnahmen, keine Messgröße für „realistisch".

### 11.4 Kosten und Grenzen

- Graph: 222 → 497 Knoten, längste Leitung 45 (`mat_gen` meldet sie jetzt, Obergrenze 90: der
  Codegen rekursiert pro Ebene, siehe `CMakeLists.txt` „MSVC main-thread stack"), 15 Parameter,
  keine Textur, `HeParams` unverändert.
- **FBM-Auswertungen pro Pixel 6 → 14** (3 Warps, 6 Chop, 1 Böen, 2 Kaustik, 2 Schaum), je vier
  Oktaven. **GPU-Zeit nicht gemessen.** Auf dem M5 ist die Aufnahme unauffällig; auf einer
  schwachen iGPU bei voller Bildschirmfläche ist das der Posten, den man als Erstes prüft. Hebel,
  falls nötig: die zweite Chop-Schicht und die Kaustik-FBM sind auf Stärke 0 entbehrlich (heute
  trotzdem gerechnet, weil es keine Static Switches im Inspector gibt, §5a).
- Der Glanz hängt am Himmel: Bei Sonne im Bild glitzert es, ohne Sonne bleibt das Wasser ein dunkler
  Spiegel mit hellen Streifen am Horizont. Das ist gewollt, ändert aber den Eindruck mit Tageszeit
  und Wolken stärker als vorher.
- Nicht belegt: D3D11/D3D12/Vulkan im Bild (nur Kompilat: he_tests, glslang, spirv-val,
  spirv-cross; `fxc` und `xcrun metal` laufen in CI), AMD/Intel, das exportierte Spiel,
  Bedienen im laufenden Editor. Der Integer-Hash-FBM ist in `test_material_graph` für alle
  Backends geprüft, im Wasser aber nicht auf D3D im Bild gesehen.
- Das Website-Handbuch (`HC-Website`, `materials.html#water`, Zweig
  `claude/eigener-wasser-shader-als-engine-default-material`) nennt noch die alten Defaults und
  zeigt das alte Bild. Anderes Repo, Deploy braucht die Bestätigung des Menschen: nicht angefasst.
