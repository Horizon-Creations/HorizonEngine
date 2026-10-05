# Wasser-Shader als Engine-Material — Plan (Thema 152)

Stand 2026-10-05, Schritt 1 (Bestandsaufnahme). Nichts am Renderer geändert.

Ziel: ein Wasser-Material als Engine-Content, gebaut als Material-Node-Graph, auf allen
fünf Backends über die bestehende Graph-Shader-Pipeline, mit vollem Parametersatz im
Inspector. Nicht Teil: Simulation, Physik/Schwimmen, Unterwasser-Postprocessing,
Ozean-Mesh-Komponente.

---

## 1. Befund: wie Default-Materialien registriert werden

<!-- EDITOR-BEFUND -->

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

<!-- BACKEND-BEFUND -->

---

## 4. Entscheidung

<!-- ENTSCHEIDUNG -->

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

UX-Kosten der Packung: der Inspector zeigt `ParamVec4` heute als vier unbeschriftete Zahlen.
Die Bedeutung der Komponenten steht deshalb im Tooltip des Param-Knotens (`tooltip`-Feld,
wird im Inspector angezeigt). Beschriftete Komponenten wären eine Editor-Änderung für
später. "Alle Parameter einstellbar" ist damit erfüllt, schön ist es noch nicht.

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

<!-- FOLGEN -->
