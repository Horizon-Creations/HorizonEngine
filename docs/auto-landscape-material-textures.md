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
  **Nachtrag Schritt 2:** Ein weiterer Sampler wurde nicht nötig. Seit Schritt 2 gibt es
  8 Layer in derselben Textur, siehe §7.
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

**Nachtrag Schritt 3:** Für die drei **Textur-Arrays**, aus denen das Auto-Material
liest, ist das gelöst. `HE::buildTextureArray` backt die Kette (8 Mips bei 128²),
und alle fünf Backends laden genau diese Level hoch (§8.3). Die 15 einzelnen
2D-Platzhalter haben weiterhin nur Mip 0.

## 7. Schritt 2: acht Layer statt vier, ohne neuen Sampler

Stand: Zweig `claude/auto-landschaftsmaterial-…`, Commit `36a27cd7` und folgende.

### 7.1 Wo die Grenze lag

| Stelle | vorher | jetzt |
|---|---|---|
| `kMatMaxLandscapeLayers` (`MaterialGraph.h`) | 4 | **8** |
| Weightmap-Daten (`TerrainComponent`) | `layerWeights`, RGBA8, Layer 0..3 | dazu `layerWeights2`, RGBA8, Layer 4..7 (leer = alles 0) |
| Paint (`TerrainPaint::paint`) | `layer > 3` → false, normiert über 4 Kanäle | 0..7, normiert über beide Seiten |
| Szenenformat | `weightRes`, `layerWeightsB64` | unverändert, dazu `layerWeights2B64` (nur wenn benutzt) |
| GPU-Textur (`TerrainSystem`) | `weightRes × weightRes` | gleich, **2·weightRes × weightRes**, sobald Layer 4..7 bemalt sind |
| Shader-Binding | `heLandscapeWeights`, Binding 14 / t14+s0 / Metal 13 / GL-Unit 13 | **unverändert**, kein neues Binding |
| MCP `terrain_paint` / `terrain_info` | Layer 0..3, Mix mit 4 Werten | 0..7, `mixAtCenter`/`layerAverage` mit 8 Werten |
| Material-Editor | „4 layers max“ | 8 |
| Neuer Blend-Knoten | 2 Layer („Layer 1/2“) | unverändert. Daher kam die Annahme „zwei Layer“. |

### 7.2 Warum ein Atlas in derselben Textur

Das Sampler-Budget ist auf D3D (SM 5.0) und Metal voll (§2.1). Eine zweite Weightmap
als eigene Textur hätte einen 17. Sampler oder ein Umschichten der Preamble-Pins in allen
fünf Backends gebraucht. Dazu ein neues Binding im Vulkan-Material-Layout und in der
D3D12-Root-Signature, also genau die beiden bekannten Fallen (Binding 14 fehlt → Vulkan
stürzt ab, Null-View bei t14 auf D3D12). Ein `sampler2DArray` hätte den Typ des
bestehenden Bindings auf allen Backends geändert, auch für jedes Vier-Layer-Material.

Darum liegen Layer 4..7 als **rechte Hälfte derselben Textur**
(`TerrainPaint::buildWeightTexture`): Zeile z = [Seite 0, Zeile z | Seite 1, Zeile z].
Der Codegen jedes Landscape Layer Blend liest die Seitenzahl am Seitenverhältnis ab:

- **quadratisch** (jedes Terrain ohne Layer 4..7 und die 1×1-Default-Map): exakt das
  alte `texture(heLandscapeWeights, vUV)`. Die Layer 4..7 sind 0.
- **2:1:** jede Hälfte bei Mip 0, U auf einen halben Texel innerhalb der Hälfte
  geklemmt. Sonst blutet beim bilinearen Filtern am Rand und in jeder Laufzeit-Mip
  (GL und Metal erzeugen eine) die andere Seite hinein.

Beide Samples werden vorab genommen und nur selektiert. So steht kein Sample mit
impliziter LOD im Kontrollfluss. Auch ein Blend mit ≤ 4 Layern liest seitenbewusst, sonst
würde er auf einer 2:1-Map beide Hälften verschmieren.

**Vertrag:** Eine Landscape-Weightmap ist immer quadratisch oder genau 2:1. Andere Formen
kommen in der Engine nicht vor (`weightRes × weightRes`). Eine von Hand gebundene 2×1-Map
würde als zwei Seiten gelesen. Der alte D3D11-WARP-Test hat deshalb jetzt eine 2×2-Map.

**Zahl 8 statt 5:** Zwei RGBA-Seiten ergeben 8 Kanäle. Mehr Sampler kostet das nicht. Die
5 Schichten des Auto-Materials (Gras, Erde, Stein, Schnee, Pfütze) passen, und es bleiben
3 Kanäle für von Hand gemalte Extras. Hang, Schnee und Pfützen sind ohnehin prozedural
(§2.2) und brauchen keinen Kanal.

**Nicht mitgewachsen, mit Absicht:** Die GI-Näherung behält 4 Layer-Farben
(`kMatApproxLayerColors`): das MTRL-Tail, `MaterialAsset::approxLayerColor[4]` und
`GiLandscape::layerColor[4]` in den GL/Metal-Kernels. Ein GI-Treffer gewichtet die Layer
0..3 nach ihrem Anteil am Paint, die Layer 4..7 gehen nur über die flache Mittelung
`approxBaseColor` ein. Wer das ändern will, muss das Asset-Format versionieren und die
GPU-Structs anfassen.

**Texturen (heTexP0..3) sind nicht Teil dieses Schritts.** Fünf Schichten × Albedo/Normal/
Maske brauchen Textur-Arrays (§2.2 Variante D). Die Queen hat dafür einen eigenen Schritt
3 eingeplant.

### 7.3 Rückwärtskompatibilität

- Eine Szene ohne `layerWeights2B64` lädt ohne zweite Seite. Upload, Shader-Pfad und
  Bild sind dann identisch zu vorher.
- Ein Terrain, das nur Layer 0..3 benutzt, schreibt keinen neuen Schlüssel. Ältere
  Builds lesen die Szene also weiter.
- Eine zweite Seite mit falscher Größe oder ohne erste Seite wird beim Laden verworfen.
- Gespeicherte Materialien regeneriert der Editor beim Laden aus dem Graph
  (`ContentManager::regenerateMaterialFromGraph`). Gepackte Builds behalten ihre
  vorkompilierten Blobs und passen weiter zu ihren eigenen (quadratischen) Maps.

### 7.4 Nachweis

**Tests (he_tests, Release, C:/hw158):**

- Paint über die Seitengrenze (Layer 3/4/5 = Index 2..4 und Index 5..7, Summe 255 über
  beide Seiten).
- Leere zweite Seite = Vier-Layer-Arithmetik, byte-gleich.
- Upload-Layout 1:1 / 2:1.
- Altes Szenenformat bleibt und lädt unverändert. Round-Trip, kaputte zweite Seite.
- `TerrainSystem` lädt 2:1 erst ab Layer 4+ hoch, unter derselben Textur-UUID.
- MCP Layer 5 + Undo.
- Codegen-Form.
- Cross-Compile mit 5 und 8 Layern für MSL, GLSL 4.10/ES 3.00/4.30, HLSL, SPIR-V.
- Register-Regel: weiterhin genau ein lebender Sampler auf s0, keine neuen t/s-Register.
- FXC + PSO gegen die volle D3D12-Material-Root-Signature mit 8 Layern (WARP).
- **D3D11-WARP-Pixeltest:**
  - 2:1-Map: links Layer 1, rechts Layer 6, in der Mitte die normierte Mischung.
  - Quadratische Map: Layer 0..3 wie vorher.
  - 3-Layer-Material auf einer 2:1-Map liest nur die linke Seite.
  - t14 leer → Layer 0.

  **Negativkontrolle:** Mit abgeschalteter Seitenerkennung schlagen genau die vier
  erwarteten Prüfungen fehl.
- Volle Suite: 4173/4175. Die 2 Fehler sind die bekannten Zwischenablage-Fälle in
  `test_inspector_ui` (OpenClipboard ist in Agent-Sitzungen verweigert).

**Hardware (NN-WS03, RTX 4070), `scripts/landscape-layers-repro/cap158.ps1` + `ana158.py`:**

- Altes Terrain (`HE_DUMP_LANDSCAPELAYERS=1`, 3 Layer, quadratische Map), merge-base
  `9a1cc950` gegen diesen Zweig: **md5-gleiche Frames** auf OpenGL und D3D11, lit und
  unlit. Der Rauschboden zweier Baseline-Läufe ist ebenfalls 0.
- Acht Layer (`HE_DUMP_LANDSCAPELAYERS=8`, Map 256×128) auf diesem Zweig: OpenGL und
  D3D11 zeigen alle sieben Scheiben. Magenta (4), Weiß (6) und Orange (7) kommen von der
  zweiten Seite.
- D3D12 und Vulkan zeigen auf diesem Zweig auch beim alten Drei-Layer-Terrain nur Layer
  0. Das liegt an der Basis: PR #95 (Vulkan-Binding 14 im Material-Layout) und PR #102
  (D3D12-Null-View bei t14) sind auf `origin/main` gemergt, aber noch nicht in diesem
  Zweig. Nachweis auf allen vier Backends deshalb in einem Scratch-Baum
  `git merge-tree origin/main HEAD` (C:/hw158m), siehe 7.5.
- `HE_DUMP_VIEWMODE=unlit` wirkt auf D3D11 nicht (Frames md5-gleich mit lit).
  Backend-Vergleiche deshalb im lit-View.
- **Metal:** keine Hardware auf diesem Gerät. Nachweis nur am Quelltext: Der MSL-Cross-
  Compile mit 5 und 8 Layern gelingt. Das 8-Layer-Fragment deklariert genau dieselben
  `[[sampler(N)]]`-Slots wie das 3-Layer-Fragment (normal und clustered), alle ≤ 15.
  Das 16er-Budget ist damit nachweislich unverändert.
- **Verhaltensänderung auf GL und Metal:** Sobald ein Terrain Layer 4+ bemalt hat (2:1-Map),
  liest es die Gewichte bei Mip 0 statt mit impliziter LOD aus der Laufzeit-Mipkette.
  Ferne Blend-Grenzen werden dort etwas schärfer. D3D und Vulkan hatten ohnehin nur Mip 0,
  dort ändert sich nichts. Für den Bildvergleich in Schritt 5 ist das eher eine
  Angleichung. Quadratische Maps lesen wie bisher.

### 7.5 Vier Backends auf origin/main + diesem Zweig

Scratch-Baum C:/hw158m aus `git merge-tree --write-tree origin/main HEAD`. origin/main
stand dabei auf `6866923d` (mit #95 und #102). Der Merge hat genau einen Konflikt, in
`tests/test_material_graph.cpp`: der Metal-Sampler-Test von main und der neue
Cross-Compile-Test stehen an derselben Stelle. Lösung: **beide behalten**, sie sind
unabhängig. Die Queen trifft beim Mergen auf denselben Konflikt.

| Witness (lit, top-down) | OpenGL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| 8 Layer, Map 256×128: alle 7 Scheiben sichtbar | ja | ja | ja | ja |
| mean\|Δ\| zu OpenGL, 8 Layer | – | 0,000 | 0,000 | 0,000 |
| mean\|Δ\| zu OpenGL, 3 Layer (altes Terrain) | – | 0,001 | 0,001 | 0,001 |

- D3D11, D3D12 und Vulkan liefern **md5-gleiche** Frames. OpenGL weicht in Einzelpixeln
  ab (Anteil > 8: 0,000 %).
- „Altes Terrain sieht gleich aus“, Beweiskette für D3D12 und Vulkan: OpenGL im
  Scratch-Baum = OpenGL auf der Zweig-Basis `9a1cc950` (mean|Δ| 0,000) = OpenGL auf
  diesem Zweig (md5). D3D12 und Vulkan im Scratch-Baum = D3D11 im Scratch-Baum (md5),
  ≈ OpenGL (0,001). Ein direktes A/B von D3D12/Vulkan gegen eine origin/main-Baseline
  **fehlt**. D3D11 hat sich durch main selbst gegenüber der Zweig-Basis um 1,7
  verschoben und liegt jetzt auf OpenGL. Der Codegen-Pfad für quadratische Maps ist auf
  allen Backends derselbe Sample wie vorher.
- Vulkan-Validation: keine Binding-14-Meldung mehr. Die eine gezählte Zeile ist die
  Info „validation layer ENABLED“.
- Toleranz für den Bildvergleich in Schritt 5: lit, mean|Δ| ≤ 1,0 und ≤ 0,5 % Pixel
  mit |Δ| > 8. Dieser Witness liegt um Größenordnungen darunter.
- Gezielte Tests im Scratch-Baum: 172/172 (Landscape, Terrain, Metal-Sampler-Vertrag
  von main, Vulkan-Layout, FXC/PSO).

## 8. Schritt 3: Textur-Arrays (sampler2DArray) auf allen fünf Backends

Stand: Zweig `claude/auto-landschaftsmaterial-…`, Commit nach `ff9c4822`. Basis ist
weiterhin `9a1cc950` + Zweig. Der Merge von origin/main wurde in dieser Sitzung von der
Rechte-Prüfung abgelehnt und ist **nicht** passiert (§8.6).

### 8.1 Was es jetzt gibt

- **Knoten:** *Texture Array Sample* (UV, Slice → RGB, A) und *Normal Map Array*
  (UV, Slice → Welt-Normale wie *Normal Map*, `p[0]` = Stärke). Beide nehmen wie
  *Texture Sample* einen Texturpfad in `s`. Ist UV offen, gilt die Mesh-UV.
  Ohne gewählte Textur (oder über dem Budget) lesen sie wie gehabt `heTex0` als 2D.
- **Codegen** (`MaterialGraph.cpp:574` ff.): Ein Array-Slot ist derselbe Slot
  `heTexPk` auf **demselben Binding/Register** (GLSL 4+k, D3D t4..t7/s4..s7, Metal
  1..4) wie ein 2D-Slot, nur als `sampler2DArray` deklariert. Pin-Tabellen,
  Vulkan-Layout und D3D12-Root-Signature bleiben unverändert. Eine Datei, die ein
  Knoten als 2D und ein anderer als Array liest, belegt zwei Slots. Der Slice wird im
  Shader gerundet und geklemmt: `clamp(floor(s + 0.5), 0, Ebenen − 1)`
  (`MaterialGraph.cpp:597`). Halbe Rundung ist zwischen APIs verschieden, und ein
  Index außerhalb ist in MSL undefiniert.
- **Wie der Renderer davon erfährt:** Der Codegen schreibt die Maske als
  `// heTexArrays <n>` direkt unter `#version 450`, **nur wenn n ≠ 0**
  (`MaterialGraph.cpp:1446`). Jeder Graph ohne Array-Knoten behält seinen Text Byte
  für Byte, also auch seine Pipeline-Caches. `HE::matGlslTextureArrayMask`
  (`MaterialGraph.cpp:1507`) liest die Maske in O(1). Die Backends nehmen sie aus
  `customShaderFragGlsl`, das unverändert in Paks wandert. Das MTRL-Format bekommt
  kein neues Feld (siehe Warnung in `HpakWriter.cpp` zur Feldsynchronisation).
- **Asset:** `TextureAsset::layers` (`Assets.h:601`). Die Slices liegen slice-major,
  jede mit eigener Mip-Kette. Das ist genau die D3D-Subresource-Reihenfolge, und
  Slice 0 steht vorn: Wer nichts von Arrays weiß, liest Slice 0 als 2D-Textur.
  Im TXMI-Tail steht `layers` nur bei > 1. Der Chunk wird dann 22 B groß und bleibt
  unter dem Legacy-Diskriminator von 24 B (`HAsset.h`, jetzt 10 von 11 freien Bytes
  belegt). Der Packer kocht Arrays nicht um (`HpakWriter.cpp:726`). Arrays sind
  **nur RGBA8**: BC7/BC3/ASTC für Arrays fehlt noch.
- **Bauen:** `HE::buildTextureArray` (`TextureArrayBuild.cpp:59`) setzt gleich große
  RGBA8-Slices zusammen und **backt die Mips** mit demselben 2×2-Box-Filter wie der
  Packer.
- **Engine-Assets:** `landscape_tex_gen` schreibt zusätzlich
  `T_Landscape_Albedo_Array` (sRGB), `T_Landscape_Normal_Array` und
  `T_Landscape_Mask_Array` (linear). Jedes hat 5 Slices in der Reihenfolge
  Grass, Dirt, Rock, Snow, WetGround, 128², 8 Mips, je 437 KB, feste UUIDs
  `0x40F..0x411`. `landscape_tex_gen <dir> --arrays-only` baut nur die Arrays neu
  aus den Einzeldateien im Ordner. Das ist der Weg, sobald die echten Texturen über
  die Platzhalter importiert sind (Packung R=AO/G=Rauheit/B=Höhe weiterhin offen,
  §3).

| Slot | Inhalt (Plan aus §2.2, jetzt umsetzbar) |
|---|---|
| `heTexP0` | `Engine/Textures/Landscape/T_Landscape_Albedo_Array.hasset` |
| `heTexP1` | `…/T_Landscape_Normal_Array.hasset` |
| `heTexP2` | `…/T_Landscape_Mask_Array.hasset` |
| `heTexP3` | frei, 2D oder Array |

### 8.2 Backends

| Backend | Array-Ressource | Leerer Array-Slot | Wo |
|---|---|---|---|
| OpenGL | `GL_TEXTURE_2D_ARRAY`, alle gespeicherten Level, kein `glGenerateMipmap` bei gebackener Kette. `BindGraphTexture` wählt das Ziel nach Speicherart (6 Bindestellen inkl. G-Buffer, transparent, Preview, UI) | weißes 1×1×1-Array | `OpenGLRenderer.cpp:7898`, `:8223` |
| Vulkan | `arrayLayers = n`, View `VK_IMAGE_VIEW_TYPE_2D_ARRAY` | `m_whiteArrayView` (gab es schon für heCsm) | `VulkanRenderer.cpp:5895` |
| D3D11 | `ArraySize = n`, SRV `TEXTURE2DARRAY` | weißes 1-Slice-Array-SRV | `D3D11Renderer.cpp:4231` |
| D3D12 | `DepthOrArraySize = n`, SRV `TEXTURE2DARRAY`. Die Null-View der Vorlage ist 2D, deshalb wird bei Array-Slots immer überschrieben | weißes 1×1×1-Array, beim ersten Bedarf hochgeladen | `D3D12Renderer.cpp:3677` |
| Metal | `MTLTextureType2DArray`, `replaceRegion:…slice:` | weißes 1×1×1-Array | `MetalRenderer.mm:8920`, hier nicht baubar, per macOS-CI kompiliert, nicht gerendert |

Jedes Backend legt die Array-Fassung eines Assets im Cache unter `"<Schlüssel>#arr"`
ab. So kann dasselbe Asset in einem Material 2D und im anderen ein Array sein.
`InvalidateTexture` verwirft beide Fassungen. Ein 2D-Asset in einem Array-Slot wird
als 1-Slice-Array hochgeladen, ein Array-Asset in einem 2D-Slot liest Slice 0.

### 8.3 Nachweis (NN-WS03, RTX 4070, Release-Build `C:\hw158`)

Witness `HE_DUMP_TEXARRAY=1` (`EditorApplication.cpp:6256`): Ein flaches 100-m-Terrain
mit **unlit** Graph-Material (also unabhängig vom Unlit-View-Mode, der auf D3D11 nicht
wirkt). Fünf Streifen über U zeigen Slice 0..4 (`slice = u·5 − 0,5`), vier Bänder
über V zeigen Albedo-Array, Normal-Array, Masken-Array und eine **2D**-Textur
(Rock-Albedo). Alle vier heTexP-Slots sind live, drei davon Arrays. Die *Normal Map
Array* hängt zusätzlich am Normal-Pin. Skripte:
`scripts/texture-array-repro/cap158arr.ps1` (eigenes APPDATA, HE_COLLAB_OFFLINE) und
`ana158arr.py` (Zellmittel 5×4, mean|Δ| über die Terrain-Box).

Zellmittel Albedo-Band, auf allen vier Backends gleich (RGB 0..255):

| Grass | Dirt | Rock | Snow | WetGround |
|---|---|---|---|---|
| (63,145,39) | (121,79,41) | (125,132,143) | (211,214,215) | (38,45,54) |

Das 2D-Band zeigt in jeder Spalte (125,132,143) wie die Rock-Slice des Arrays, und
das Masken-Band trennt WetGround über Rauheit G = 126 von den anderen (≈ 220).

| gegen OpenGL | D3D11 | D3D12 | Vulkan | D3D12 mit Debug-Layer + DRED |
|---|---|---|---|---|
| größte Abweichung eines Zellmittels | 0,36 | 0,36 | 0,36 | 0,36 |
| mean\|Δ\| Terrain | 0,171 | 0,171 | 0,171 | 0,171 |
| Pixel mit \|Δ\| > 8 | 0,338 % | 0,338 % | 0,338 % | 0,338 % |

- D3D11, D3D12 und Vulkan liefern untereinander dieselben Werte. Die Toleranz aus §7.5
  (mean|Δ| ≤ 1,0, ≤ 0,5 % Pixel > 8) ist eingehalten.
- **Negativkontrolle 1** (`HE_DUMP_TEXARRAY=slice2`, jeder Streifen liest Slice 2):
  Auf allen vier Backends werden alle Albedo-/Masken-Zellen zu Rock, mean|Δ| zum
  Normalbild 17,5–17,7, größte Zellabweichung 104. Die Slice-Auswahl ist also echt
  und nicht immer Slice 0.
- **Negativkontrolle 2** (Vulkan, absichtlich 2D-View im Array-Slot, nur lokal gebaut,
  danach zurückgesetzt): Die Validation meldet sofort `VkImageViewType is
  VK_IMAGE_VIEW_TYPE_2D but the OpTypeImage has (Dim = 2D) and (Arrayed = 1)` für
  heTexP0..2. Die **null** Validation-Meldungen des echten Laufs sind also belastbar.
  Nach dem Zurücksetzen ist das Bild identisch zum ersten Lauf (mean|Δ| 0,000).
- D3D12 mit `HE_GPU_DEBUG=1` (Debug-Layer + DRED an): keine Meldung, gleiches Bild.
- D3D11 hat keinen Debug-Layer im Baum. Dort gilt nur der Bildbefund.
- **Metal:** keine Hardware auf diesem Gerät. Belegt sind nur: MSL-Cross-Compile
  (`texture2d_array<float>` an den Array-Slots), dieselben `[[texture(N)]]`- und
  `[[sampler(N)]]`-Slots wie bei vier 2D-Texturen (normal + clustered), also kein
  zusätzlicher Sampler. **macOS-CI** (Run 37591669224 auf `6cae02b3`, Job macOS: success):
  `MetalRenderer.mm` kompiliert, ctest 230/230 inkl. `test_material_graph` mit den neuen
  Array-Tests, und der Schritt „Compile-check the runtime MSL strings“ ist grün. Gerendert
  hat Metal damit noch **nicht**: CI erzeugt kein Bild, ein Metal-Bildvergleich steht aus.
- Tests: Codegen/Maske/Slot-Trennung, Cross-Compile für MSL, GLSL 4.10/ES 3.00/4.30,
  HLSL und SPIR-V, Aufnahme in alle Node-Sweeps (FXC wie D3D11/D3D12 kompilieren,
  D3D12-Root-Signature, GL-Link), Array-Asset-Roundtrip mit 22-B-TXMI.

### 8.4 Speicher in 2K

Ein Array mit 5 Slices à 2048² RGBA8 plus Mips ist ≈ 5 × 21,3 MiB = 107 MiB, die drei
Arrays zusammen ≈ 320 MiB. Das gehört **nicht** in git (siehe §4.4, kein LFS). Die
Arrays sind abgeleitete Daten: Mit `--arrays-only` entstehen sie aus den 25 PNGs
(nach Import und Packung) neu. Die 128²-Platzhalter-Arrays (3 × 437 KB) sind
eingecheckt, damit Schritt 4/5 ohne den Menschen weiterlaufen.

### 8.5 Was offen bleibt

- **BC7/BC3/ASTC für Arrays:** Der Packer lässt Arrays unverändert. Für 2K ist
  Kompression aber nötig (§5). Das braucht `cookTexture` pro Slice und die
  Block-Pfade in den vier Array-Uploads.
- **Pack-Modus für die echten Einzel-PNGs** → `T_Landscape_*_Mask` (§3): weiter offen.
- **Metal** ist per CI gebaut und getestet (§8.3), aber auf keiner Hardware gerendert.
- D3D12 bindet einen **leeren 2D**-Slot weiterhin als Null-View, also Schwarz, während
  die anderen Backends Weiß binden. Das gab es schon vorher und betrifft keinen
  Array-Slot.

### 8.6 Zweig-Basis

Der Zweig steht weiter auf `9a1cc950`, vor #95/#102. Der Merge von origin/main war von
der Queen freigegeben, wurde aber in dieser Sitzung von der Rechte-Prüfung abgelehnt.
Er fehlt also und muss von der Queen oder dem Menschen nachgeholt werden. Für die
Array-Slots ist das ohne Belang: Sie liegen auf t4..t7 bzw. Binding 4..7, nicht auf
14. Bemaltes Terrain zeigt auf diesem Zweig auf D3D12/Vulkan aber weiterhin nur
Layer 0 (§7.5). Beim Merge sind Konflikte in den Renderern rund um die
Graph-Textur-Bindung möglich: D3D12 `resolveGraphTexture`/`srvForTexture`, Vulkan
`heTexP`-Block.
