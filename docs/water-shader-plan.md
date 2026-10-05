# Wasser-Shader als Engine-Material — Plan (Thema 152)

Stand 2026-10-05, Schritt 1 (Bestandsaufnahme), auf Bitte des Leitstands vor Abschnitt 3 beendet. Nichts am Renderer geändert.

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
| **Wellen** | Bausteine da: `Panner`, `Noise`, `FBM Noise`, `Noise Texture`, `Sine`, `WorldPos`, `Normal Map` (Tangentenraum → Welt ohne Vertex-Tangenten), WPO-Pin (Vertex-Wellen) | `MaterialGraph.h:38–60`, `MaterialGraph.cpp:614` |
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

**Offen — Schritt 1 wurde auf Bitte des Leitstands vor diesem Abschnitt beendet.**
Fest steht nur: `MatBlendMode::Translucent` existiert und leitet ins sortierte
Alpha-Blend-Pass (`MaterialGraph.h:174`), und `heLitP` hängt die Himmelsreflexion an
`heLight.fog.z` (laut Kommentar `MaterialShaderLibrary.cpp:675–680` setzte Vulkan `fog.z`
zumindest früher nicht). Noch zu belegen, pro Backend mit Datei:Zeile: Gibt es den
Translucent-Pass für **Graph**-Materialien? Blendfaktoren, Depth-Write aus/-Test an? Läuft
er mit Lit-Preamble (`heSkyEnv`, SSR, CSM)? Im Deferred-Pfad nach dem Resolve forward
gezeichnet? Wird `fog.z` und `sunDir.w` (Zeit) überall gesetzt? Welche Bindungen/Slots
sind frei für SceneColor/SceneDepth (Metal-Fragment ist am 16-Sampler-Limit, HLSL-SM5-Pin-
Tabelle in `MaterialShaderLibrary::fragment`)? Wie macht `Backdrop` das im UI-Pass?

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

- **Vor Schritt 2:** Abschnitt 3 nachziehen (Backend-Tabelle). Davon hängt ab, ob ein
  Translucent-Graph-Wasser auf allen fünf Backends überhaupt gezeichnet und gespiegelt wird.
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
