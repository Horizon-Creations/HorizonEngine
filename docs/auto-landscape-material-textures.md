# Auto-Landschaftsmaterial: Textur-Liste

Thema 158, Schritt 1 · Stand main `9a1cc950` · 2026-10-07

Was der Mensch für das automatische Terrain-Material besorgen muss, und warum genau
so viel und nicht mehr. Bis die echten Texturen da sind, laufen alle weiteren
Schritte mit den Platzhaltern aus §6.

**Kurzfassung für den Einkauf:** 5 Schichten × 5 Maps = **25 PNG-Dateien**, alle
**2048 × 2048**, nahtlos kachelbar, CC0, Normal-Maps in **OpenGL-Konvention**.
Nicht mehr. Die Liste mit Dateinamen steht in §4.

---

## 1. Wie das Terrain heute Texturen bindet

Es gibt **keine Landscape-Layer-Assets**. Ein Layer ist nur ein Name an einem
*Landscape Layer Blend*-Knoten im Material-Graph. Seine Farbe kommt aus dem, was
der Nutzer an den Eingang des Layers hängt (typisch ein *Texture Sample*):

- **Gewichte:** `TerrainComponent::layerWeights` ist eine RGBA8-Weightmap mit
  `weightRes` = 256 Texeln Kantenlänge. Kanal k gehört zu Layer k
  (`src/HE_Scene/include/HorizonScene/Components/TerrainComponent.h:61-62`).
  Der Terrain-Paint-Pinsel schreibt in diese Weightmap
  (`src/HE_Scene/include/HorizonScene/TerrainPaint.h:7`). Der Renderer bindet sie
  pro Draw als `heLandscapeWeights` (GLSL-Binding 14). Auf Metal ist das Sampler 13,
  auf OpenGL Unit 13, auf D3D t14/s0
  (`src/HE_Rendering/src/material/MaterialShaderLibrary.cpp:2525`, `:2640`;
  `src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp:3586`).
- **Layer-Grenze:** `kMatMaxLandscapeLayers = 4`, eine RGBA8-Weightmap
  (`src/HE_Core/include/MaterialGraph/MaterialGraph.h:162`). Die Annahme
  „vermutlich nur zwei Layer“ aus dem Thema stimmt also nicht: **heute sind es 4**.
  Mehr bräuchte eine zweite Weightmap und damit einen weiteren Sampler (Schritt 2).
- **Unbemaltes Terrain:** Es bekommt die 1×1-Weightmap (255,0,0,0) und zeigt
  Layer 0 (`src/HE_Core/include/ContentManager/DefaultAssets.h:46-50`).
  Das Default-Terrain-Material ist untexturiert grau (`DefaultAssets.h:33-36`).
- **Texturen pro Material:** Ein Material hat höchstens **4 Projekttexturen**
  (`kMatMaxGraphTextures = 4`, `MaterialGraph.h:406`; Shader-Namen
  `heTexP0..3`). Dazu kommt der Legacy-/Mesh-Slot `heTex0`. Jede weitere Textur
  fällt still auf `heTex0` zurück (`src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:552-566`).
  Alle Layer eines Blend-Knotens teilen sich diese 4 Slots.

Die eigentliche Engstelle für dieses Thema ist deshalb **nicht die Layerzahl**,
sondern **die Zahl der Textur-Slots**.

## 2. Sampler-Budget

### 2.1 Was schon belegt ist

Shader Model 5.0 (D3D11/D3D12) kennt die Sampler-Register s0..s15. Metal erlaubt
im Fragment-Stage ebenfalls nur 16 Sampler. **Beide Budgets sind heute voll**
(`MaterialShaderLibrary.cpp:2468-2481` für Metal, `:2577-2648` für D3D):

| Register (D3D) | belegt durch | gehört |
|---|---|---|
| s0 | `heLandscapeWeights` (live) + `heAO` (nur deklariert, wird per texelFetch gelesen) | Engine |
| s1, s3 | DDGI-Irradiance / -Visibility | Engine |
| s2 | `heTex0` (Legacy-/Mesh-Textur) | Material |
| s4..s7 | `heTexP0..3` (Projekttexturen des Graphs) | **Material** |
| s8, s9 | Forward-SSR / GI-Reflexion | Engine |
| s10, s11 | GI-Schattenmasken Sonne / lokal | Engine |
| s12 | CSM-Array | Engine |
| s13 | Schattenatlas für Punkt-/Spotlichter | Engine |
| s14 | Wolkenschatten | Engine |
| s15 | Sky-Env-Cubemap | Engine |

Die Engine braucht also 11 Register, für das Material bleiben **5**: `heTex0` und
`heTexP0..3`. Für ein selbst gebautes Material sind praktisch nur die **4** Slots
`heTexP0..3` frei wählbar. Ein 17. Sampler lässt sich nicht einfach dazupinnen:
FXC lehnt den Shader dann ab (X4509/X4500), Metal baut die Pipeline nicht, und
jedes Graph-Material fällt still auf das eingebaute PBR zurück.

OpenGL vergibt Units bis 20 (`OpenGLRenderer.cpp:3562-3600`) und hängt am Treiber
(NVIDIA: 32). Vulkan hat deutlich höhere Grenzen. **Maßgeblich sind Metal und
D3D mit SM5.0.**

### 2.2 Bedarf des Auto-Materials

Für jede Schicht werden drei Texturen gesampelt: Albedo, Normal und eine gepackte
Maske (§3). Dazu kommt die Weightmap, die schon im Engine-Budget steckt.

| Variante | Slots pro Schicht | Schichten bei 4 Slots | reicht für 5? |
|---|---|---|---|
| A: einzelne 2D-Texturen, AO/Rauheit/Höhe **ungepackt** (5 Maps je Schicht) | 5 | 0 (nicht mal eine) | nein |
| B: einzelne 2D-Texturen, **gepackte** Maske (3 je Schicht) | 3 | 1 | nein |
| C: nur Albedo, kein Normal, keine Maske | 1 | 4 | nein |
| **D: Textur-Arrays** (ein Array je Map-Art, Schicht = Array-Slice) | 3 für **alle** Schichten | beliebig | **ja, 1 Slot bleibt frei** |

**Ergebnis:** Fünf Schichten mit Normal und Rauheit gehen nur mit Textur-Arrays,
also `Texture2DArray` / `sampler2DArray`. Ein Array belegt ein Sampler-Register,
egal wie viele Slices es hat. Arrays kann der Graph-Codegen heute **nicht**:
`heTexP0..3` sind einfache `sampler2D`. Das muss ein späterer Schritt auf allen
fünf Backends nachrüsten (Codegen, Bindings, Asset-Seite). Dieser Schritt ändert
weder Shader noch Formate.

Belegung mit Arrays:

| Slot | Inhalt | Farbraum |
|---|---|---|
| `heTexP0` | Albedo-Array, 5 Slices | sRGB |
| `heTexP1` | Normal-Array, 5 Slices | linear |
| `heTexP2` | Masken-Array, 5 Slices: R = AO, G = Rauheit, B = Höhe | linear |
| `heTexP3` | frei (Reserve, z. B. später Wellen-Normal für Pfützen) | – |

Textur-Bombing braucht **keine** zusätzliche Textur, die Zufallswerte kommen aus
einem Hash im Shader. Hang-, Höhen- und Pfützenverteilung sind prozedural
(Steigung, Höhe, flache Mulden) und brauchen **keinen** Weightmap-Kanal. Kanäle
brauchen nur Schichten, die man von Hand malen will.

### 2.3 Obergrenze für den Einkauf

- **5 Schichten**: Gras, Erde, Stein, Schnee, nasser Boden. Keine Varianten
  (kein „Stein 2“). Gegen sichtbare Wiederholung arbeitet das Bombing, nicht
  zusätzliche Texturen.
- **5 Maps je Schicht**: Albedo, Normal, Rauheit, AO, Höhe. Kein Metallic
  (Terrain ist nicht metallisch), kein Displacement-EXR, keine zweite
  Normal-Map in DX-Konvention.
- **Höchstens 25 Dateien.** Mehr Sampler gibt das Budget nicht her. Mehr Speicher
  auch nicht, siehe §5.

## 3. Packung: ja, AO + Rauheit + Höhe in eine Maske

Ja, packen ist nötig. Ohne Packung wären es 5 Slots je Schicht (Variante A). Mit
Packung sind es 3 (Varianten B/D).

- **Der Mensch liefert die Maps einzeln** (§4), so wie sie Poly Haven oder
  ambientCG ausliefern. Gepackt wird engine-seitig, das muss niemand von Hand machen.
- **Kanalbelegung der Maske:** R = AO, G = Rauheit, B = Höhe, A = 255. R/G folgen
  der glTF-ORM-Reihenfolge (Occlusion R, Roughness G). B ist Höhe statt Metallic,
  denn Terrain hat kein Metall. Das Asset heißt deshalb `…_Mask`, **nicht**
  `…_ORM`/`…_ARM`, weil der Name sonst B falsch beschreibt.
- Die Höhe liegt in der Maske und nicht im Alpha der Albedo. Das hält die sRGB-Map
  sauber, und alle Daten-Maps bleiben zusammen in einer linearen Textur.
- Wer packt: Die Platzhalter (§6) liegen schon gepackt vor. Für die echten
  Texturen fehlt noch ein Pack-Schritt, z. B. ein Modus von `landscape_tex_gen`,
  der die Einzel-PNGs liest. Das ist offen und gehört zu einem späteren Schritt.

## 4. Die Liste

### 4.1 Für alle Dateien

| Eigenschaft | Vorgabe | Warum |
|---|---|---|
| Auflösung | **2048 × 2048**, quadratisch, Zweierpotenz | 1K wirkt aus Bodennähe weich. 4K vervierfacht den Speicher (§5). |
| **Alle Schichten gleich groß** | ja, ausnahmslos | Slices eines Textur-Arrays müssen dieselbe Größe und dasselbe Format haben. |
| Kachelbar | **nahtlos in beide Richtungen** | Das Terrain kachelt jede Schicht viele Male. |
| Drehbar | ohne Vorzugsrichtung: kein gerichtetes Licht, keine in eine Richtung liegenden Halme | Bombing dreht und versetzt Kacheln zufällig. |
| Keine Einzelstücke | kein auffälliger Einzelstein, keine einzelne Blume | Ein Unikat verrät die Wiederholung, auch mit Bombing. |
| Albedo „delit“ | ohne eingebackene Schatten, AO oder Glanzlichter | Schatten und AO rechnet die Engine selbst. |
| Dateiformat | **PNG**, 8 oder 16 Bit | Der Importer liest über stb_image und wandelt in RGBA8 (`src/HE_Tools/src/AssetImporter/TextureImporter.cpp:49-50`). EXR/TIFF kann er nicht. JPG zerstört Normal-Maps. |
| Lizenz | **CC0** (Poly Haven, ambientCG) | Darf ins Spiel und in den Engine-Content. Quelle je Schicht notieren. |
| Abgedeckte Fläche | Gras/Erde/nasser Boden ca. 2 m × 2 m, Stein/Schnee ca. 4 m × 4 m | Nur ein Anhalt, damit die Maßstäbe zusammenpassen. Die Kachelung ist später ein Material-Parameter. |

### 4.2 Pro Map-Art

| Map | Farbraum | Inhalt | Achtung |
|---|---|---|---|
| `Albedo` | **sRGB** | RGB = Grundfarbe, Alpha wird ignoriert | „BaseColor“/„Diffuse“ von der Quelle, umbenennen |
| `Normal` | **linear** | Tangent-Space, **OpenGL-Konvention: Grün = +Y (oben)** | Poly Haven `nor_gl`, ambientCG `NormalGL`. **Nicht** `nor_dx`/`NormalDX`. |
| `Roughness` | linear | Graustufen, 0 = glatt, 1 = rau | Liefert die Quelle nur „Gloss“, muss das noch invertiert werden: Bescheid geben, nicht stillschweigend einsortieren. |
| `AO` | linear | Graustufen, 1 = unverdeckt | – |
| `Height` | linear | Graustufen, 0 = tief, 1 = hoch, über die Kachel normiert | Wird für die Höhen-Überblendung zwischen Schichten und für Pfützen in den Fugen gebraucht. 16 Bit ist willkommen, wird aber auf 8 Bit gelesen. |

**Woher die Normal-Konvention kommt:** Der Importer spiegelt beim Laden vertikal
(`flipVertically = true`, „bottom-left UV origin“, `src/HE_Tools/src/AssetImporter/TextureImporter.h:13`).
Der *Normal Map*-Knoten baut seinen Tangentenrahmen aus den Bildschirm-Ableitungen
der gespeicherten UV, also mit +V = „oben im Bild“
(`MaterialGraph.cpp:614-627`, `:1325-1333`). Deshalb gilt: Grün = +Y, und es wird
nie gekippt. Der PBR-Importer hält das ausdrücklich fest und testet es
(`src/HE_Tools/src/AssetImporter/PbrMaterialImport.cpp:525-531`,
`tests/test_gltf_material_import.cpp`). Eine Map in DX-Konvention würde jede Beule
als Delle zeigen.

**Woher der Farbraum kommt:** Er steht als `srgb`-Flag pro Textur-Asset
(`src/HE_Core/include/ContentManager/Assets.h:592`). Beim Import rät
`suggestTextureSrgb` (`src/HE_Tools/src/AssetImporter/ImporterCommon.cpp:190`)
anhand von Namens-Bausteinen: `normal`, `roughness`, `ao`, `height`, `mask` →
linear, alles andere → sRGB. Der erste Baustein (die Schicht) zählt nicht mit.
**Die Dateinamen unten sind so gewählt, dass jede Datei automatisch den richtigen
Farbraum bekommt.** Wer umbenennt, riskiert eine Normal-Map, die als sRGB gelesen
wird.

### 4.3 Die 25 Dateien

Schichtnamen in ASCII-Englisch, Schema `<Schicht>_<Map>.png`:

| Schicht | Albedo (sRGB) | Normal (lin., GL) | Rauheit (lin.) | AO (lin.) | Höhe (lin.) | Motiv |
|---|---|---|---|---|---|---|
| Gras | `Grass_Albedo.png` | `Grass_Normal.png` | `Grass_Roughness.png` | `Grass_AO.png` | `Grass_Height.png` | kurzes Wiesengras, gern mit etwas Erde dazwischen |
| Erde | `Dirt_Albedo.png` | `Dirt_Normal.png` | `Dirt_Roughness.png` | `Dirt_AO.png` | `Dirt_Height.png` | trockener Waldboden / Erde mit kleinen Steinen |
| Stein | `Rock_Albedo.png` | `Rock_Normal.png` | `Rock_Roughness.png` | `Rock_AO.png` | `Rock_Height.png` | **Felswand / gebrochener Fels**, ohne Moos. Hauptteil der automatischen Verteilung, also die wichtigste Schicht. |
| Schnee | `Snow_Albedo.png` | `Snow_Normal.png` | `Snow_Roughness.png` | `Snow_AO.png` | `Snow_Height.png` | verharschter Schnee, leicht verweht, nicht reinweiß (Albedo ca. 0,8) |
| Nasser Boden | `WetGround_Albedo.png` | `WetGround_Normal.png` | `WetGround_Roughness.png` | `WetGround_AO.png` | `WetGround_Height.png` | nasser Schlamm / matschige Erde (Rand und Grund von Pfützen) |

Das **Wasser der Pfütze** ist keine Textur. Es entsteht prozedural: flache Normale,
Rauheit ≈ 0,05 und abgedunkelte Albedo über dem, was darunter liegt. Eine
Wellen-Normal-Map ist **nicht** nötig (Wetter-Kopplung und Animation gehören nicht
zum Thema). Der freie Slot `heTexP3` hält sie sich nur offen.

### 4.4 Zielordner, offene Entscheidung

Die Platzhalter liegen als Engine-Assets in
`EditorDeps/EngineContent/Textures/Landscape/` (§6). Materialien sprechen sie über
das reservierte Präfix `Engine/` an
(`Engine/Textures/Landscape/T_Landscape_<Schicht>_<Map>.hasset`,
`src/HE_Core/src/ContentManager/ContentManager.cpp:936`). Die echten Texturen
ersetzen genau diese Dateien. **Einfach importieren reicht dafür nicht:** Ein
normaler Import von `Grass_Albedo.png` schreibt `Grass_Albedo.hasset` (Name aus
dem Quell-Stamm) und legt eine neue UUID an. Ersetzt wird auf einem dieser Wege:

- mit einem Import, dessen Ausgabe-Ziel ausdrücklich auf
  `T_Landscape_Grass_Albedo.hasset` zeigt. Ein Import auf eine vorhandene Datei
  behält deren UUID (`ImporterCommon.cpp:322-324`) und deren sRGB-Flag
  (`ImporterCommon.cpp:845`).
- oder mit dem noch fehlenden Pack-Schritt (§3). Er liest die Einzel-PNGs und
  schreibt die `T_Landscape_*`-Dateien mit denselben festen UUIDs
  (`hi` = 0x400 + Index, §6).

Weil die UUIDs fest sind, bleiben die Verweise aus Materialien heil, egal welches
Werkzeug die Dateien neu schreibt.

**Offen, das entscheidet der Mensch: wohin mit den großen Dateien?** Das Repo hat
**kein Git LFS**, und Binär-Ballast wurde schon einmal aus der Historie gepurgt.
In 2K belegen die 15 Engine-Assets (RGBA8, ohne Mips) etwa 15 × 16 MiB = 240 MiB,
die 25 Quell-PNGs zusammen grob 100–250 MB. Möglichkeiten:

1. Engine-Assets über die EngineContent-SFTP-Veröffentlichung verteilen
   (`src/HE_ContentSync`, Editor: „Publish Engine Content to Server“), nicht über git.
2. Quell-PNGs in einen Ordner, den git ignoriert, z. B.
   `EditorDeps/EngineContent/Textures/Landscape/Source/`, und nur die kleinen
   Platzhalter in git lassen.
3. Git LFS einführen (größerer Eingriff, betrifft alle).

Bis das entschieden ist: **die echten Texturen nicht committen.**

## 5. Speicher

| Auflösung | je Slice RGBA8 + Mips | 15 Slices RGBA8 | 15 Slices BC7 (Pack/Cook) |
|---|---|---|---|
| 1K | 5,3 MiB | 80 MiB | 20 MiB |
| **2K** | 21,3 MiB | **320 MiB** | **80 MiB** |
| 4K | 85,3 MiB | 1,25 GiB | 320 MiB |

Im Editor liegen Texturen als RGBA8 vor. BC7/ASTC entstehen erst beim
Packen (`TextureFormat`, `Assets.h:565`). 2K ist der Kompromiss. 4K lohnt sich
für ein kachelndes Terrain nicht.

## 6. Platzhalter

`EditorDeps/EngineContent/Textures/Landscape/` enthält 15 kleine Texturen, je Schicht
eine Albedo, eine Normal und eine Maske. Sie sind 128 × 128 groß, bereits im
Engine-Format (`.hasset`, RGBA8, nur Mip 0, siehe Warnung unten) und bereits
gepackt wie in §3:

| Schicht | Albedo-Grundton | Rauheit (G der Maske) | Höhenmuster |
|---|---|---|---|
| Grass | grün | 0,85 | weiche Hügel |
| Dirt | braun | 0,90 | flache Hügel |
| Rock | blaugrau | 0,70 | hohe Blöcke |
| Snow | fast weiß | 0,60 | sehr flach |
| WetGround | dunkles Graublau | 0,15 | flach mit Mulden |

Jede Albedo ist ein 4 × 4-Schachbrett aus zwei Tönen der Schichtfarbe. Die Zelle
links unten trägt eine helle **L-Marke**. An ihr sieht man im Bild, ob Kacheln
wiederholt, versetzt oder gedreht werden: der Nachweis fürs Bombing. Die
Normal-Map ist aus der Höhe abgeleitet (GL-Konvention, wie §4.2), passt also zur
Maske.

Asset-Namen und feste UUIDs (`hi` = 0x400 + Index, `lo` = 1, Index = Schicht × 3 +
Map, Map 0/1/2 = Albedo/Normal/Mask):

```
T_Landscape_Grass_Albedo      T_Landscape_Grass_Normal      T_Landscape_Grass_Mask
T_Landscape_Dirt_Albedo       T_Landscape_Dirt_Normal       T_Landscape_Dirt_Mask
T_Landscape_Rock_Albedo       T_Landscape_Rock_Normal       T_Landscape_Rock_Mask
T_Landscape_Snow_Albedo       T_Landscape_Snow_Normal       T_Landscape_Snow_Mask
T_Landscape_WetGround_Albedo  T_Landscape_WetGround_Normal  T_Landscape_WetGround_Mask
```

Erzeugt werden sie deterministisch mit `landscape_tex_gen`
(`src/HE_Tools/src/LandscapeTexGen/main.cpp`), nach demselben Muster wie
`mesh_gen`/`widget_gen`/`matfn_gen`. Ein zweiter Lauf schreibt byte-gleiche Dateien:

```
landscape_tex_gen EditorDeps/EngineContent/Textures/Landscape
```

Ins Editor-Deploy kommen sie nur, wenn `HorizonEditor` neu linkt. Erst dann
kopiert sein POST_BUILD ganz `EditorDeps/` (`src/HE_Editor/CMakeLists.txt:442-447`).
macOS packt ganz `EngineContent/` (`scripts/package_macos.sh:190`).

**Warnung für den Backend-Vergleich (Schritt 5): Mips sind nicht gleich.** Eine
Textur mit `mipLevels = 1` bekommt auf OpenGL und Metal zur Laufzeit eine
Mip-Kette (`OpenGLRenderer.cpp:7867`, `MetalRenderer.mm:12621`). D3D11, D3D12
und Vulkan laden dagegen genau die gespeicherten Level hoch
(`D3D11Renderer.cpp:4236`, `D3D12Renderer.cpp:3662`, `VulkanRenderer.cpp:5901`),
also nur Mip 0. Das betrifft die Platzhalter und genauso jede normal importierte
Textur, denn `TextureImporter` schreibt ebenfalls nur Level 0. Fernes Terrain
flimmert dann auf D3D und Vulkan und ist auf GL und Metal gefiltert. Ein
Bildvergleich scheitert so aus einem Grund, der mit dem Material nichts zu tun hat.
Abhilfe, offen für einen späteren Schritt:
- die Mip-Kette vorab backen, im Generator bzw. Pack-Schritt oder im Importer
  (GL nimmt eine vorgebackene Kette schon an, `OpenGLRenderer.cpp:7849`)
- oder auf D3D und Vulkan beim Hochladen Mips erzeugen
