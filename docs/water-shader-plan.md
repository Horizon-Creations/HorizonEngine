# Wasser-Shader als Engine-Material — Plan (Thema 152)

Stand 2026-10-05, Schritt 1 (Bestandsaufnahme), vollständig. Nichts am Renderer geändert; Abschnitt 3 nennt Renderer-Fehler, die spätere Schritte beheben müssen.

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
