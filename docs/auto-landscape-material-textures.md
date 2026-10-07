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
- **Pro Band** (D3D11/D3D12/Vulkan gegen GL, alle drei gleich): Albedo-Array
  mean|Δ| 0,006, Normal-Array 0,000, Masken-Array 0,040 (0,015 % > 8). Die gesamte
  Restabweichung sitzt im **2D**-Band: 0,637, 1,33 % > 8. Das ist der bekannte
  Mip-Unterschied der 2D-Assets ohne gebackene Kette (§6). Die gebackenen Array-Mips
  sind also wirklich auf allen Backends dieselben.
- **Fallback-Pfade** (`HE_DUMP_TEXARRAY=fallback`, alle vier Backends, D3D12 mit
  Debug-Layer, Vulkan mit Validation). Jedes Band ergibt das erwartete Bild:
  fehlendes Array → weißes Array (231,231,231); 2D-Asset im Array-Slot → 1-Slice-Array,
  jeder Slice auf 0 geklemmt → Rock in allen Streifen; echtes Albedo-Array → die fünf
  Schichten; Array-Asset im **2D**-Slot → Slice 0 (Grass) in allen Streifen. Die einzige
  Fehlermeldung ist das absichtlich fehlende Asset, keine Validation- oder
  Debug-Layer-Meldung. D3D/Vulkan gegen GL: mean|Δ| 0,170, die Abweichung liegt wieder
  nur im Band mit dem mip-losen 2D-Asset.
- **GL deferred** (`HE_DUMP_RENDERPATH=1`): pixelgleich zu forward (mean|Δ| 0,000).
  `HE_DUMP_GBUFFER=2` zeigt das Terrain im G-Buffer, die G-Buffer-Variante mit Maske und
  ihre Bindestelle sind also gelaufen. D3D/Vulkan haben noch keinen Deferred-Pfad
  (Thema 150).
- D3D11 hat keinen Debug-Layer im Baum. Dort gilt nur der Bildbefund.
- **Metal:** keine Hardware auf diesem Gerät. Belegt sind nur: MSL-Cross-Compile
  (`texture2d_array<float>` an den Array-Slots), dieselben `[[texture(N)]]`- und
  `[[sampler(N)]]`-Slots wie bei vier 2D-Texturen (normal + clustered), also kein
  zusätzlicher Sampler. **macOS-CI** (Run 37591669224 auf `6cae02b3`, Job macOS: success):
  `MetalRenderer.mm` kompiliert, ctest 230/230 inkl. `test_material_graph` mit den neuen
  Array-Tests, und der Schritt „Compile-check the runtime MSL strings“ ist grün. Gerendert
  hat Metal damit noch **nicht**: CI erzeugt kein Bild, ein Metal-Bildvergleich steht aus.
  **Nachtrag Schritt 5 (M5, Metal gerendert):** Zuerst zeigte Metal nur die erste
  Kachel, weil heTexP mit einem klemmenden Sampler gebunden war (§10.3). Nach dem Fix
  `86601d4d` gegen OpenGL auf demselben Gerät: Zellmittel max. 0,39, mean|Δ| 0,168,
  0,322 % > 8. Pro Band: Albedo-Array 0,000, Normal-Array 0,000, Masken-Array 0,038,
  2D-Band 0,632 / 1,27 %. Das ist dasselbe Bild wie D3D11/D3D12/Vulkan oben.
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

## 9. Schritt 4: Textur-Bombing als Material-Knoten

Stand: Zweig `claude/auto-landschaftsmaterial-…`, Commits `fef4bd08` (Knoten),
`a99157ba` (Cell-Pin + Zeuge) und der Normal-Rahmen-Fix danach (§9.3). Die
Zweig-Basis ist unverändert (§8.6).

### 9.1 Was es jetzt gibt

- **Vier Knoten** in der Kategorie *Texture*:

  | Knoten | Eingänge | Ausgänge |
  |---|---|---|
  | *Texture Bombing* | UV, Cell | RGB, A |
  | *Normal Map Bombing* | UV, Cell | N (Welt) |
  | *Texture Array Bombing* | UV, Slice, Cell | RGB, A |
  | *Normal Map Array Bombing* | UV, Slice, Cell | N (Welt) |

  Sie belegen **dieselben heTexP-Slots** wie *Texture Sample* / *Texture Array
  Sample* (Pfad in `s`). Es gibt also keinen neuen Sampler, kein neues Binding und
  keine Änderung an Pin-Tabelle, Vulkan-Layout oder D3D12-Root-Signature. Graphen
  ohne Bombing-Knoten behalten ihren Shader-Text Byte für Byte.
- **Parameter:** `p[0]` Drehung (0..1 von ±180°, 0 = nur Versatz), `p[1]`
  Blend-Schärfe (Exponent, ≤ 0 → 7), `p[2]` Seed (gerundet, negative Werte laufen
  wie im Shader als `uint` um), `p[3]` Stärke (nur Normal-Knoten). **Cell** ist der
  Abstand der Hex-Mitten in Textur-Wiederholungen, ungebunden 0,5
  (`kMatBombDefaultCell`). Als Pin kann ein Parameter ihn pro Schicht steuern.
  Das Paper skaliert fest mit 2√3 = 0,29 Wiederholungen. Damit zerfallen grosse
  Strukturen wie Felsblöcke in kleine Stücke, deshalb ist der Wert hier einstellbar.
- **Verfahren:** Hex-Tiling nach Mikkelsen (JCGT 2022). Die UV-Ebene wird in ein
  Dreiecksgitter geschert, die drei Ecken unter dem Pixel sind die Mitten der drei
  Hexe, die hier mischen. Jedes Hex liest mit eigenem Versatz (0..1 Wiederholung)
  und eigener Drehung um seine Mitte. Gewicht = baryzentrisch hoch Schärfe,
  normiert. Ein Lesezugriff kostet **drei** Texturzugriffe.
- **Gleich auf allen Backends:** Der Zufall kommt aus einem **Integer-Hash**
  (pcg3d) des Hex-Index, nicht aus `fract(sin())`. Ein anderer Versatz wäre ein
  anderes Texel. Der Index geht float → int → uint, und die 24-Bit-Werte werden
  exakt nach float umgerechnet.
- **Keine Mip-Sprünge an den Nähten:** Alle Zugriffe laufen über `textureGrad` mit den
  Gradienten des **ungebombten** UV, gedreht mit dem Hex. Im HLSL wird das
  `SampleGrad`, im MSL `gradient2d`; das prüft ein Test.
- **Normalen:** Die Tangentenraum-Normale jedes Hex wird mit `transpose(R)`
  zurückgedreht und erst dann gemischt. Danach folgt der Rahmen des *Normal
  Map*-Knotens mit dem ungedrehten UV.
- **Ein Gitter pro Schicht:** Der Codegen merkt sich das Gitter pro Scope +
  UV-Ausdruck + Cell-Ausdruck + Drehung/Schärfe/Seed. Albedo, Normal und Maske
  einer Schicht mit gleichen Werten teilen **ein** `heBombGrid` und liegen damit
  deckungsgleich. Die Zeugen-Logzeile meldet „1 hex grid(s)“ für drei Knoten.
- **Bewusst weggelassen:** Mikkelsens Luminanz-Gewichtung der Farbe. Sie würde dem
  Albedo andere Gewichte geben als der Normal-Map und der Maske derselben Schicht,
  dann lägen die drei nicht mehr deckungsgleich.
- Editor (Rot/Blend/Seed/Strength mit Hilfetexten, Textur-Drop und -Picker) und MCP
  (Typnamen `*BombSample`, `requires` mit der p-Belegung, Pfadprüfung).
  `HE::matNodeSamplesTexture` ersetzt die verstreuten Vier-Knoten-Listen.

### 9.2 Nachweis (NN-WS03, RTX 4070, Release-Build `C:\hw158`)

Zeuge `HE_DUMP_TEXBOMB` (`EditorApplication.cpp`, nach dem TEXARRAY-Zeugen): dasselbe
flache, **unlit** 100-m-Terrain, 5 Streifen = Slices, Textur-Tiling 10. Vier Bänder:

1. plain Albedo-Array (Referenz, wiederholt sich je Kachel)
2. gebombtes Albedo-Array
3. (N.x, Höhe, N.z): gebombte Normal-Map-Array-Lesung um die Höhe (B) der
   gebombten Masken-Lesung
4. gebombtes Masken-Array

Skripte: `scripts/texture-bombing-repro/cap158bomb.ps1` (Aufruf wie §8.3) und
`ana158bomb.py`. Das Skript misst:

- **Wiederholung:** mean|I(p) − I(p + 1 Kachel)|; die Periode wird an Band 1 angepasst
  (67 × 67 px).
- **Hang-Korrelation:** corr(Bild-Gradient der Höhe, N.x bzw. N.z) pro Streifen.
  Die Platzhalter-Normale *ist* die Steigung der Höhe. Die Korrelation bleibt also
  nur stark, wenn Normale und Höhe auf denselben Hexen liegen **und** jede Normale
  richtig zurückgedreht ist.

| Modus `=1`, OpenGL | Wiederholung (h / v) |
|---|---|
| plain Albedo | 0,60 / 0,80 |
| gebombtes Albedo | 12,97 / 13,59 |
| N.x / Höhe / N.z | 8,02 / 7,89 |
| gebombte Maske | 5,28 / 5,56 |

Die Zellmittel des gebombten Albedo sind gleich denen des plain Albedo (größte
Abweichung 2): Bombing verschiebt den Farbton nicht.

| Hang-Korrelation `=1` | Grass | Dirt | Rock | Snow | WetGround |
|---|---|---|---|---|---|
| corr(dH/dx, N.x) | −0,892 | −0,920 | −0,896 | −0,910 | −0,866 |
| corr(dH/dy, N.z) | −0,888 | −0,922 | −0,898 | −0,902 | −0,871 |

Auf D3D11, D3D12 und Vulkan sind alle Werte dieselben.

| gegen OpenGL (nach §9.3) | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| `=1` mean\|Δ\| Terrain / Pixel > 8 | 0,002 / 0,000 % | 0,002 / 0,000 % | 0,002 / 0,000 % |
| `=off` | 0,003 / 0,000 % | 0,003 / 0,000 % | 0,003 / 0,000 % |
| `=seed7` | 0,002 / 0,000 % | 0,002 / 0,000 % | 0,002 / 0,000 % |
| `=mismatch` | 0,002 / 0,000 % | 0,002 / 0,000 % | 0,002 / 0,000 % |

Die Toleranz aus §7.5 (mean|Δ| ≤ 1,0, ≤ 0,5 % Pixel > 8) ist damit weit
unterschritten.

- **Negativkontrolle `=off`** (Bänder 2–4 plain gelesen): Die Wiederholung fällt auf
  0,9–2,3, die Hang-Korrelation der plain Normal-Map liegt bei −0,91…−0,97. Bombing
  kostet also etwas Korrelation (Blend-Zonen), aber kein Vorzeichen.
- **Negativkontrolle `=mismatch`** (Normal auf Seed 1, Höhe auf Seed 0, die Logzeile
  meldet 2 Gitter): Die Korrelation fällt auf −0,04…+0,08, auf allen vier Backends
  gleich. Das Mass erkennt also, ob Normale und Albedo/Maske auf demselben Gitter
  liegen.
- **`=seed7`:** Die gebombten Bänder ändern sich vollständig gegenüber `=1` (mean|Δ|
  Albedo 12,4, 54 % > 8). Band 1 bleibt gleich bis auf die Grenzzeilen der Bänder
  (0,23, dieselbe Grösse wie `=off` gegen `=1`). Die Wiederholung bleibt bei
  12,4–12,9.
- Vulkan mit Validation-Layer (`validation layer ENABLED` im Log): keine einzige
  WARN- oder ERROR-Zeile ausser „No config file“.
- Vor dem Fix in §9.3 zeigte das gebombte Albedo-Band auf D3D/Vulkan 0,075 / 0,8 %
  > 8 gegen GL, nach dem Fix 0,003 / 0,000 %. Der Fix berührt den Albedo-Pfad
  nicht. Vermutlich optimiert der Shader-Compiler das geteilte Gitter anders. Beide
  Werte liegen innerhalb der Toleranz, die Ursache ist aber nicht geklärt.
- **Metal:** keine Hardware. Belegt sind der MSL-Cross-Compile (normal, clustered,
  G-Buffer) mit `gradient2d` an jedem Zugriff und dieselben Slots wie bei den
  Array-Knoten. Gerendert hat Metal die Knoten nicht.
- Tests: `test_material_graph.cpp` mit drei neuen Fällen (Gitter-Teilung,
  Integer-Hash, Gradienten-Zugriffe, Cell im Gitter-Schlüssel, Fallback auf `heTex0`,
  Parameter-Säuberung, JSON; Cross-Compile MSL/GLSL 4.10/ES 3.00/4.30/HLSL/SPIR-V)
  und der Aufnahme in die FXC-/D3D12-Root-Signature-/GL-Sweeps. Die
  Registry-Schleife lief bisher nur bis `WindSway`, die Array-Knoten aus Schritt 3
  waren dort gar nicht dabei; jetzt reicht sie bis zum letzten Knoten.

### 9.3 Nebenbefund: Normal Maps waren auf D3D/Vulkan gespiegelt (behoben)

Der Zeuge hat einen Fehler gefunden, den es schon vor dem Bombing gab. Der Rahmen
`hePerturbNormal` aller Normal-Map-Knoten spiegelte auf D3D11/D3D12/Vulkan N.x und
N.z gegenüber OpenGL: Hang-Korrelation +0,93 statt −0,93, auch mit `=off`, also ganz
ohne Bombing. GL ist physikalisch richtig. Die Kamera bei Yaw 0 / Pitch −89 legt
Bild-rechts auf +X und Bild-unten auf +Z, und die Normale muss vom Hang wegkippen
(N.x ∝ −∂H/∂X). Das sieht der Schritt-3-Zeuge nicht, weil sein Normal-Band rohe
Texel zeigt und keine gestörte Normale.

- **Ursache:** Schülers Cotangent-Frame enthält das Vorzeichen der
  Jacobi-Determinante Bildschirm ↔ uv. `dFdy` läuft auf GL nach oben, auf D3D, Vulkan
  und Metal nach unten. Auf jedem Front-Face dort kippte damit der ganze Rahmen.
- **Fix** (`MaterialGraph.cpp`, `hePerturbNormal`):
  `invmax *= dot(cross(dp1, dp2), N) < 0.0 ? -1.0 : 1.0;`. Das Produkt enthält
  genau das Vorzeichen der Bildschirm-Basis und nichts von den UVs. Damit ist T =
  ∂p/∂u auf jedem Backend und auch auf Back-Faces. Gespiegelte UVs spiegeln den
  Rahmen weiterhin, wie es der glTF-Test verlangt. Der Fix braucht keine
  Backend-Konvention und deckt deshalb auch Metal ab, das hier nicht laufen kann.
- **Gates:**
  - GL vorher/nachher bitgleich (mean|Δ| 0,000 für `=1` und `=off`), denn
    GL-Front-Faces haben das Vorzeichen +1.
  - D3D11/D3D12/Vulkan danach mit GL-Vorzeichen; das Normal-Band gegen GL fällt
    von 4,33 auf 0,000, das ganze Terrain von 1,10 / 13,3 % > 8 auf
    0,002 / 0,000 %.
  - `test_gltf_material_import.cpp` hat jetzt einen Fall mit nach unten laufender
    Bildschirm-Y-Achse, der das Ergebnis von GL liefern muss.
- **Reichweite:** Jedes Graph-Material mit Normal Map (auch die importierten
  PBR-Graphen aus `PbrMaterialImport`) wird auf D3D/Vulkan/Metal jetzt richtig
  herum beleuchtet, auf GL ändert sich nichts. Im Editor erzeugt der ContentManager
  Basis-Graph-Materialien beim Laden neu (`ContentManager.cpp`, „regenerate the baked
  GLSL from the graph“), alte Assets bekommen den Fix also von selbst. Gepackte Spiele
  mit vorkompilierten Shader-Blobs behalten den alten Stand bis zum nächsten Export.
  Eingebaute und Decal-Shader bauen keinen solchen Ableitungs-Rahmen (grep
  `dp2perp`), sie sind nicht betroffen.

### 9.4 Für Schritt 5

- Pro Schicht kosten Albedo + Normal + Maske gebombt 9 Texturzugriffe. Fünf Schichten
  voll gebombt wären also 45, gegenüber 15 ohne Bombing. Sinnvoll ist: nur die
  Schichten bomben, die auch Gewicht haben (Steigung, Schnee, Pfütze entscheiden
  das vorher), oder für Schnee und nassen Boden plain lesen.
- Pro Schicht eigenen Seed nehmen, damit die Muster der Schichten nicht gemeinsam
  wiederkehren. Albedo/Normal/Maske **einer** Schicht brauchen denselben Seed und
  dieselbe Cell-Quelle.
- Cell 0,5 ist ein Startwert. Für die echten 2K-Texturen am Bild prüfen.
- Die Hilfe zu „+ Layer“ spricht noch von vier Layern als Grenze. Das stimmt seit
  Schritt 2 nicht mehr (acht). **Nachtrag Schritt 5:** korrigiert.

## 10. Schritt 5: das Auto-Landschaftsmaterial

Stand: Zweig `claude/auto-landschaftsmaterial-…`, Commits `86601d4d` (Metal-Sampler),
`df36842c` (Material, Generator, Zeuge, Tests) und die Doku danach. Gemessen auf dem
MacBook Air (Apple M5), Release-Build, **Metal und OpenGL**. D3D11/D3D12/Vulkan sind
auf diesem Gerät nicht lauffähig; das ist Schritt 6 auf NN-WS03.

### 10.1 Was es jetzt gibt

- **Engine-Material** `Engine/Materials/M_AutoLandscape.hasset`, feste UUID
  `0x412/1` (`HE::kAutoLandscapeMaterialId`). Es ist ein **gewöhnlicher Material-Graph**
  aus Standardknoten (151 Knoten): kein eigener Knotentyp und kein Backend-Code. Auf allen
  fünf Backends läuft also derselbe Codegen. Der Graph öffnet sich im Material-Editor wie
  jeder andere, und eine Material-Instanz stellt ihn über 14 Parameter ein.
- **Quelle** ist `HE::buildAutoLandscapeGraph` (`src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp`).
  Das Asset ist generiert, nicht von Hand gespeichert:
  `landscape_tex_gen EditorDeps/EngineContent/Materials --material`. Ein zweiter Lauf
  schreibt dieselben Bytes. Der Test „The shipped M_AutoLandscape.hasset is exactly what
  the builder makes“ schlägt an, wenn jemand den Builder ändert und den Generator
  vergisst.
- **Texturen:** die drei Arrays aus §8 in heTexP0..2, `heTexP3` bleibt frei. Kein
  Weightmap-Kanal, keine Paint-Layer: Die Verteilung ist vollständig prozedural.
- **Benutzen:** das Material aus dem Content Browser (Ordner *Engine/Materials*) auf
  ein Landscape ziehen, oder eine Material-Instanz davon anlegen und z. B. *Snow Height*
  an die Höhe der eigenen Welt anpassen.

### 10.2 Wie verteilt wird

Pro Pixel, in dieser Reihenfolge (jede Stufe ist ein Lerp über Albedo, Normale und
Maske):

| Stufe | Maske | Parameter (Vorgabe) |
|---|---|---|
| Boden | Gras, darauf Erde in fBm-Flecken (Welt-Rauschen) **plus** ein Erdgürtel knapp unter der Felsgrenze (Geröll) | *Dirt Amount* 0,35, *Dirt Patch Size* 24 m |
| Fels | `smoothstep(Rock Slope, + Rock Blend, slope)`, slope = 1 − N.y der **geometrischen** Normale | *Rock Slope* 0,12 (≈ 28°), *Rock Blend* 0,12 (voll bei ≈ 40°) |
| Schnee | Welthöhe über *Snow Height*, über *Snow Blend* geschlossen, nicht auf Flächen steiler als *Snow Max Slope* (dort bleibt Fels) | 60 m, 6 m, 0,45 (≈ 57°) |
| Pfützen | flacher Boden (slope < *Puddle Max Slope*), ohne Schnee, in den **Senken eines zweiten Welt-Rauschfelds**: nasser Rand (Wet-Ground-Schicht) und in der Mitte stehendes Wasser | *Puddle Amount* 0,32, *Puddle Size* 10 m, *Puddle Max Slope* 0,03 (≈ 14°) |
| Wasser | Albedo × 0,35, Rauheit 0,05, Normale = geometrische Normale | fest |

- **Stein ist der Hauptteil** der automatischen Verteilung: Er beginnt schon bei ≈ 28°,
  also auf jedem nennenswerten Hang.
- **Höhen-Überblendung** (*Height Blend*, Vorgabe 1): Jeder Übergang wird um die
  Höhendifferenz der beteiligten Schichten (Masken-B) verschoben. Hohe Fels-Texel
  stechen vor der Steigungsgrenze durch das Gras. Hoher Schnee deckt zuerst. Wasser füllt
  zuerst die tiefen Texel des nassen Bodens.
- **Kachelung im Welt-Raum** (*Ground Tile Size* 2 m für Gras/Erde/nassen Boden,
  *Rock Tile Size* 4 m für Fels/Schnee, wie §4.1), nicht über das 0..1-UV des Terrains.
  Ein Texel ist damit auf einem 100-m- und einem 4-km-Landscape gleich groß.
  `TerrainComponent::uvTiling` wirkt auf dieses Material deshalb nicht.
- **Bombing** (§9) für Fels, Gras und Erde: je ein Hex-Gitter pro Schicht, geteilt von
  Albedo/Normal/Maske, eigener Seed (11/23/37), *Bombing Cell* 0,5. Schnee und nasser
  Boden werden plain gelesen (§9.4). Damit sind es 33 statt 45 Texturzugriffe. Der
  Static Switch **„Texture Bombing“** schaltet die gebombten Zugriffe zur Compile-Zeit
  auf plain. Eine Instanz mit dem Schalter aus ist eine eigene Permutation; der
  *Bombing Cell*-Parameter fällt dort heraus (13 statt 14 Slots).
- **„Mulde“ heißt hier: Senke des Rauschfelds, nicht Senke des Terrain-Meshes.** Der
  Shader kennt keine Krümmung des Geländes, und das Terrain hat keine Vertex-Farbe.
  Pfützen liegen also auf flachem Boden zufällig verteilt, nicht gezielt in echten
  Geländemulden. Echte Mulden bräuchten einen Kavitäts-Kanal, den TerrainSystem aus der
  Höhe backt (Vorschlag: eigenes Thema; ein Weightmap-Kanal oder ein Vertex-Attribut).

### 10.3 Nebenbefund: Metal hat Graph-Material-Texturen nie gekachelt (behoben)

Metal hat hier **zum ersten Mal** Array- und Bombing-Knoten gerendert (Schritte 3/4 nur
Cross-Compile). Das erste Bild zeigte das Terrain in vier Kacheln mit falschen Farben,
entlang x = 0 und z = 0 getrennt. Auch der TEXARRAY-Zeuge aus §8.3 zeigte auf Metal
nur die erste Kachel, danach den Randtexel verschmiert, und das auch im 2D-Band.

- **Ursache:** `MetalRenderer` band heTexP0..3 mit `m_linearSampler`, dessen
  Adressmodus der Default **ClampToEdge** ist. GL (Default-Wrap), D3D (WRAP) und Vulkan
  (REPEAT) kacheln diese Slots. Jedes Graph-Material mit UV außerhalb 0..1 (jede
  UV-Kachelung > 1) war auf Metal falsch, nicht erst das Auto-Material.
- **Fix** `86601d4d`: `m_materialSampler` (linear + Mips wie bisher, Repeat) an allen
  sechs heTexP-Bindestellen (Vorschau, UI, forward, transparent, G-Buffer). heTex0
  (Slot 0) bleibt beim klemmenden Sampler; ob eingebaute Mesh-Texturen mit UV > 1 auf
  Metal ebenfalls klemmen, ist **nicht geprüft** (offen).
- **Nachweis:** TEXARRAY-Zeuge auf Metal nach dem Fix = das GL-Bild (Kacheln, L-Marken,
  alle Bänder). Auto-Material Metal gegen GL siehe §10.4.

### 10.4 Nachweis (Metal + OpenGL, M5, Release)

Zeuge `HE_DUMP_AUTOLAND` (`EditorApplication.cpp`): ein **analytisches** 128-m-Relief
auf y = 300, konstant entlang Z: Ebene (x < −24), Smoothstep-Rampe bis ≈ 62° und
zurück, 40-m-Plateau (x > 8). *Snow Height* steht auf y = 320. Jede Spalte hat damit
einen bekannten Sollzustand. Modi: `1` (das **ausgelieferte Asset**, per Pfad geladen,
über eine Material-Instanz mit überschriebener *Snow Height*; das prüft feste UUID,
`Engine/`-Präfix, Regenerieren beim Laden und den Instanz-Pfad), `nobomb` (dieselbe
Instanz mit dem Schalter aus), `masks` / `ground` / `normal` / `surface` (unlit:
Masken, finale Normale, AO/Rauheit), `builtin` und `plaingraph` (Kontrollen ohne
Auto-Material). Skripte: `scripts/auto-landscape-repro/cap158auto.sh` (macOS),
`cap158auto.ps1` (Windows, gleiche Kamera), `ana158auto.py` (masks / diff / repeat).
Aufnahme top-down von y = 400, TOD 0,4, Wolken/GI/SSAO/SSR/AA/Bloom aus, forward,
`HE_SKY_TIME=30`. Zwei Läufe desselben Builds sind bitgleich.

**Soll/Ist pro Region** (`ana158auto.py masks`, Anteil Pixel > 128, Metal = GL):

| Region | Fels | Schnee | Wasser | Erde | nass |
|---|---|---|---|---|---|
| Ebene x −60..−30 | 0 % | 0 % | 18,3 % | 33,2 % | 30,6 % |
| Hangfuß x −23..−21 | 27 % | 0 % | 1,2 % | 93,5 % | 2,3 % |
| Hang x −16..−9 (55–62°) | 100 % | 0 % | 0 % | – | 0 % |
| Plateau x 16..60 (y 340) | 0 % | 100 % | 0 % | 2,8 % | 0 % |

Alle 15 Erwartungen des Skripts sind erfüllt: Fels am Hang, Schnee in der Höhe, Wasser
nur auf flachem Boden und nie unter Schnee, Erdgürtel am Hangfuß.

**Metal gegen OpenGL** (mean|Δ| / Anteil Pixel mit |Δ| > 8, Terrain-Spalten):

| Modus | gesamt | Ebene | Hangfuß | Hang | Plateau |
|---|---|---|---|---|---|
| `masks`, `ground`, `normal`, `surface` | 0,000 / 0 % | 0,000 | ≤ 0,002 | ≤ 0,001 | 0,000 |
| `1` mit Schatten-Distanz 0,1 m | **0,001 / 0,000 %** | 0,001 | 0,003 | 0,002 | 0,000 |
| `nobomb` mit Schatten-Distanz 0,1 m | 0,001 / 0,000 % | 0,001 | 0,008 | 0,007 | 0,000 |
| `1` schräg (Kamera −24°), Schatten 0,1 m | 0,006 / 0,000 % | | | | |
| `1` mit Schatten | 2,30 / 4,38 % | 0,001 | **70,6 / 100 %** | 1,14 / 5,6 % | 0,000 |
| `plaingraph` (nur graues Graph-Material) mit Schatten | 3,47 / 4,41 % | 0,000 | **108,8 / 100 %** | 0,000 | 0,000 |
| `builtin` (Default-Terrain-Material) mit Schatten | 0,17 / 0,44 % | 0,000 | 0,000 | 0,000 | 0,000 |

- **Das Material ist auf Metal und GL gleich:** Masken, finale Normale, AO und Rauheit
  sind bitgleich, das beleuchtete Bild ohne Schatten auch (0,001 / 0 %, Toleranz §7.5:
  ≤ 1,0 / ≤ 0,5 %).
- **Mit Schatten nicht**, und das liegt **nicht** am Material: Metal zeichnet bei
  Graph-Materialien einen Schatten an den Hangfuß (und körnig auf den unteren Hang), den
  GL nicht zeichnet. Das passiert auch mit einem Graph-Material, das nur eine konstante
  Farbe ist (`plaingraph`), aber **nicht** mit dem eingebauten Terrain-Material auf
  Metal selbst (`builtin`). Drei von vier Pfaden (GL eingebaut, GL Graph, Metal
  eingebaut) stimmen überein; welche Seite physikalisch richtig ist, wurde nicht aus dem
  Sonnenstand hergeleitet. Die körnige Form am unteren Hang sieht nach Schatten-Akne aus.
  Der Fehler sitzt damit sehr wahrscheinlich im Schatten-Lookup von Metals
  Graph-Beleuchtung (heLitP), jedenfalls nicht im Codegen dieses Themas. Ein längerer
  Schattenabstand (`SHADOW=400`) ändert nichts. Das ist ein eigener Fehler und braucht
  ein eigenes Thema. Bis dahin vergleicht Schritt 6 das Material mit
  `-Extra @{HE_DUMP_SHADOW='0.1'}` und zusätzlich mit Schatten, um zu sehen, ob D3D/Vulkan
  sich wie GL oder wie Metal verhalten.
- **Bombing bricht die Wiederholung** (`ana158auto.py repeat`, Ebene, Lumen-Differenz
  bei Verschiebung um k = 4..40 px): ohne Bombing Minima bei 6, 12, 19, 25, 31, 37 px
  (1-m-Schachfeld und 2-m-Kachel bei ≈ 6,2 px/m), Oszillation der Kurve 1,58 (h) /
  1,45 (v). Mit Bombing kein periodisches Minimum mehr (nur k = 4) und Oszillation
  0,12 / 0,12, also gut 12-mal weniger. Auf Metal und GL dieselben Zahlen. Gebombt
  gegen plain unterscheiden sich die gebombten Regionen deutlich (Ebene 9,9 / 54 % > 8,
  Hang 9,7 / 62 %), das Plateau (Schnee, immer plain) gar nicht.
- Keine `[ERROR]`-, Link- oder Compile-Zeile in einem der 20 Läufe. Die
  Draw-Counter melden 4 Draws (2 × 2 Chunks) in jedem Modus.
- Tests (`test_material_graph.cpp`, alle grün im Release): Budgets (3 Array-Slots,
  Maske 7, 14 Parameter, kein heTex0-Fallback, keine Paint-Layer), 3 Hex-Gitter,
  Bombing-aus-Permutation, Debug-Ansichten unlit, JSON-Roundtrip; Cross-Compile von
  `lit`, `nobomb`, `masks`, `ground` für MSL, GLSL 4.10/ES 3.00/4.30, HLSL, SPIR-V,
  clustered und G-Buffer, mit ≥ 27 Gradienten-Zugriffen auf Metal und HLSL; Aufnahme in
  die FXC/D3D12-Root-Signature/GL-Link-Sweeps (laufen nur unter Windows); Wächter für
  das ausgelieferte Asset.

### 10.5 Was offen bleibt

- **D3D11, D3D12, Vulkan:** nicht gerendert, nur der Cross-Compile in den Tests (HLSL,
  SPIR-V). Das ist Schritt 6 auf NN-WS03 mit `cap158auto.ps1`. Dort fehlt auch der
  Zweig-Merge mit origin/main (§8.6). Für dieses Material ist das unerheblich, weil es
  heLandscapeWeights nicht liest.
- **Metal-Schatten bei Graph-Materialien** (§10.4): eigener Fehler, eigenes Thema.
- **Echte Geländemulden für Pfützen** (§10.2): Kavitäts-Kanal, eigenes Thema.
- **heTex0 auf Metal:** ob Slot 0 (eingebaute Mesh-Textur) bei UV > 1 auch klemmt, ist
  nicht geprüft.
- **Steile Felswände** werden über Welt-XZ projiziert und strecken sich ab ≈ 60°. Das
  Bombing verdeckt es teilweise. Triplanar/Biplanar für Fels würde 2–3-mal so viele
  Zugriffe kosten. Erst mit den echten Texturen am Bild entscheiden.
- **Malbare Overrides** (Layer-Blend „Auto/Grass/Rock/Snow/Puddle“, um Pfützen oder Fels
  von Hand zu setzen): bewusst weggelassen, weil Paint auf D3D12/Vulkan auf diesem Zweig
  bis zum Merge nur Layer 0 zeigt (§7.5).
- Die Vorgaben (Steigungen, Pfützenmenge, Kachelgrößen) sind an den Platzhaltern
  gewählt und müssen mit den echten Texturen am Bild nachgestellt werden.
- GI-Näherung: `matGraphApproxSurface` kann Texturknoten nicht falten
  (`approxFoldNode` → `false`, „textures … cannot fold“). Die DDGI-Farbrückstrahlung
  bekommt für dieses Material also den Rückfallwert statt einer Mischung der
  Schichtfarben. Wie stark das im Bild auffällt, ist nicht geprüft.
