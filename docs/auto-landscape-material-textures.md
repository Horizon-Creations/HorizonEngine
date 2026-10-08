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
  **Nachtrag Thema 177:** Der Modus heißt `landscape_tex_gen --pack` (§4.4), davor sitzt
  `scripts/landscape-textures/stage_polyhaven.py` für EXR und 4K (§14).

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
| Nasser Boden | `WetGround_Albedo.png` | `WetGround_Normal.png` | `WetGround_Roughness.png` | `WetGround_AO.png` | `WetGround_Height.png` | nasser Schlamm / matschige Erde (Rand und Grund von Pfützen). **Entfällt seit Thema 177, Schritt 3** (§16): Der nasse Rand ist ein Overlay auf der Schicht darunter, das Auto-Material liest diese Schicht nicht mehr. Wer das Set trotzdem liefert, wird gepackt, aber nicht gelesen. |

Das **Wasser der Pfütze** ist keine Textur. Es entsteht prozedural: flache Normale,
Rauheit ≈ 0,05 und abgedunkelte Albedo über dem, was darunter liegt. **Seit Schritt 3
von Thema 177 gilt das auch für den nassen Rand** (§16). Eine
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

**Packen der echten Texturen (`landscape_tex_gen --pack`).** Der Pack-Schritt aus §3
existiert jetzt als Modus des Werkzeugs:

```
landscape_tex_gen <Ziel> --pack <PNG-Ordner> [--size 2048]
```

Er liest `<Schicht>_<Map>.png` aus einem flachen Ordner (Schichten Grass, Dirt, Rock,
Snow, WetGround; Maps Albedo, Normal, Roughness, AO, Height; auch jpg/tga/bmp). Die
Quellnamen von ambientCG/Poly Haven werden mit angenommen: `Color`/`BaseColor`/`Diffuse`
für Albedo, `NormalGL` für Normal, `AmbientOcclusion` für AO, `Displacement` für Height.
Er schreibt `T_Landscape_<Schicht>_{Albedo,Normal,Mask}.hasset` mit den festen UUIDs
(Maske: R = AO, G = Rauheit, B = Höhe) und setzt danach die drei `_Array`-Texturen
zusammen. Fehlendes AO wird Weiß, fehlende Höhe Mittelgrau, eine Schicht ganz ohne
Dateien behält ihr Platzhalter-Aussehen (auf die Pack-Größe hochskaliert), und Maps
anderer Größe werden auf `--size` skaliert (die Meldung steht im Log).

Für **ein Projekt allein** ohne das Repo anzufassen: als `<Ziel>` den Override-Ordner
`<Projekt>/Content/Engine/Textures/Landscape` angeben. Ein Projekt-Override hat dieselbe
UUID und schlägt den mitgelieferten Platzhalter (§1 im Abschnitt `resolveAbsolutePath`).

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
- ~~**Pack-Modus für die echten Einzel-PNGs** → `T_Landscape_*_Mask` (§3)~~: erledigt,
  `landscape_tex_gen --pack` (§4.4), mit den echten Texturen benutzt in §14.
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
| Pfützen | flacher Boden (slope < *Puddle Max Slope*), ohne Schnee, in den **Senken eines zweiten Welt-Rauschfelds**: nasser Rand und in der Mitte stehendes Wasser. **Overlay auf der Schicht darunter, keine eigene Schicht** (§16) | *Puddle Amount* 0,32, *Puddle Size* 10 m, *Puddle Max Slope* 0,03 (≈ 14°) |
| Nasser Rand | Albedo × 0,6, Rauheit × 0,5; Normale und AO bleiben die des Bodens | fest |
| Wasser | Albedo × 0,35 des **trockenen** Bodens, Rauheit 0,05, Normale = geometrische Normale | fest |

- **Stein ist der Hauptteil** der automatischen Verteilung: Er beginnt schon bei ≈ 28°,
  also auf jedem nennenswerten Hang.
- **Höhen-Überblendung** (*Height Blend*, Vorgabe 1): Jeder Übergang wird um die
  Höhendifferenz der beteiligten Schichten (Masken-B) verschoben. Hohe Fels-Texel
  stechen vor der Steigungsgrenze durch das Gras. Hoher Schnee deckt zuerst. Wasser füllt
  zuerst die tiefen Texel des Bodens darunter (Masken-B von Gras/Erde an dieser Stelle).
- **Kachelung im Welt-Raum** (*Ground Tile Size* 2 m für Gras/Erde,
  *Rock Tile Size* 4 m für Fels/Schnee, wie §4.1), nicht über das 0..1-UV des Terrains.
  Ein Texel ist damit auf einem 100-m- und einem 4-km-Landscape gleich groß.
  `TerrainComponent::uvTiling` wirkt auf dieses Material deshalb nicht.
- **Bombing** (§9) für Fels, Gras und Erde: je ein Hex-Gitter pro Schicht, geteilt von
  Albedo/Normal/Maske, eigener Seed (11/23/37), *Bombing Cell* 0,5. Schnee wird
  plain gelesen (§9.4). Damit sind es **30 statt 36** Texturzugriffe (27 gebombt + 3 Schnee;
  bis Thema 177, Schritt 3, waren es 33 mit dem plain gelesenen nassen Boden). Der
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
  **Richtigstellung aus Schritt 6 (§11.3):** Diese Deutung war falsch herum. Metal hat
  recht, der Fuß liegt analytisch im Schlagschatten der steileren Rampe. Das eingebaute
  Material zeichnet ihn auf **beiden** Backends, deshalb unterscheidet sich `builtin` nicht.
  Den Schatten lässt nur **OpenGL im Forward-Pfad bei Graph-Materialien** weg, und zwar
  absichtlich (`csmSplits.w = 0`).
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
- ~~**Metal-Schatten bei Graph-Materialien**~~ (§10.4): kein Metal-Fehler. Es fehlt der
  Sonnenschatten bei GL-Forward-Graph-Materialien, siehe §11.3.
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

## 11. Schritt 6: Verifikation über die Backends

Stand: Zweig bei `1b8776db` + diesem Schritt. Gemessen auf dem MacBook Air (Apple M5,
macOS 27, Stromsparmodus), Release-Build `out/build/macos-release` (shaderc ON, Tests ON).

### 11.1 Was auf welchem Backend geprüft ist

| Backend | Gerät / Treiber | gerendert? | Nachweis |
|---|---|---|---|
| Metal | Apple M5, echte Hardware | ja | §11.2, alle Modi |
| OpenGL 4.1 | Apple M5 (Apple-GL über Metal), echte Hardware | ja | §11.2, alle Modi |
| D3D11 | – | **nein** | nur Cross-Compile + FXC-Sweep in den Tests (Windows-CI) |
| D3D12 | – | **nein** | nur Cross-Compile + Root-Signature-Sweep in den Tests (Windows-CI) |
| Vulkan | – | **nein** | nur SPIR-V-Cross-Compile in den Tests |

- **D3D11, D3D12 und Vulkan sind nicht gerendert.** Auf diesem Mac gibt es keinen
  D3D-Treiber. Der lavapipe-Job (`vulkan-lavapipe`, `scripts/he_vk_imagetests.py`) liegt
  nur auf main; der Zweig steht 111 Commits dahinter (Basis `9a1cc950`, 02.10.), und den
  Merge darf ein Arbeiter nicht selbst machen. Auch per CI gibt es von diesem Zweig aus
  also keine Vulkan-Pixel. Die Queen hat die Messung auf NN-WS03 (RTX 4070, echte
  Hardware) als Folgeschritt zugesagt, sobald dort ein Platz frei ist; das Rezept steht
  in §11.4.
- **Vollbau:** `cmake --build out/build/macos-release -j8`, rc 0, keine `error:`-Zeile.
  Vorher wurden alle 38 Quelldateien, die der Zweig ändert, per `touch` neu übersetzt
  (Falle aus Schritt 5: ein Edit mitten im Compile hinterlässt ein altes `.o`). Danach war
  der Deploy `out/deploy/Editor` md5-gleich mit dem Build (Editor + 9 dylibs).
- **Tests** (Release, `ctest -j4`, im Vordergrund abgewartet): 17/17 grün:
  `test_material_graph` (193 s), `test_terrain`, `test_terrain_tessellation`,
  `test_terrain_heightmap`, `test_terrain_generate`, `test_terrain_tools_ui`,
  `test_mcp_tools_terrain`, `test_mcp_tools_material`, `test_contentmanager`,
  `test_hpak`, `test_scene_serializer`, `test_scene_autosave`, `test_asset_autosave`,
  `test_gltf_material_import`, `test_culling`, `test_texture_colour_space`,
  `test_texture_orientation`. Ein eigener Savegame-Test existiert nicht. Das
  Landscape-Format prüft `test_terrain`: „A four-layer landscape writes the old scene
  format and an old scene loads unchanged“ und „Layers 4..7 round-trip through the
  scene file, a broken second page is dropped“.
- **CI** auf `1b8776db` (Lauf 37629204081): Windows, Linux und macOS grün. Windows:
  231/231 Tests, darunter `test_material_graph` (351 s) mit den FXC-, D3D12-Root-Signatur-
  und GL-Link-Sweeps. Der Fall „Auto landscape material“ läuft im `_WIN32`-Block mit.
  Das ist der einzige D3D-Beleg dieses Schritts. Er beweist, dass der Shader kompiliert,
  nicht, dass das Bild stimmt.

### 11.2 Metal gegen OpenGL

Gleiche Aufnahme wie §10.4 (`cap158auto.sh`, top-down, TOD 0,4, `HE_SKY_TIME=30`, AA,
GI, SSAO, SSR, Bloom aus). 36 Läufe, kein `[ERROR]`, Link- oder Compile-Fehler, in jedem
Lauf 4 Draws (forward) bzw. 5 (deferred). Toleranz wie §7.5: mean|Δ| ≤ 1,0 **und**
≤ 0,5 % Pixel mit |Δ| > 8.

| Modus | Pfad | gesamt | Ebene | Hangfuß | Hang | Plateau | Urteil |
|---|---|---|---|---|---|---|---|
| `masks` / `ground` / `normal` / `surface` | forward | 0,000 / 0 % | 0,000 | ≤ 0,002 | ≤ 0,001 | 0,000 | bitgleich bis auf Rundung |
| `1`, Schatten 0,1 m | forward | 0,001 / 0 % | 0,001 | 0,003 | 0,002 | 0,000 | ok |
| `nobomb`, Schatten 0,1 m | forward | 0,001 / 0 % | 0,001 | 0,008 | 0,007 | 0,000 | ok |
| `1`, Schatten 0,1 m | deferred | 0,001 / 0 % | 0,001 | 0,004 | 0,003 | 0,000 | ok |
| `1` mit Schatten | **deferred** | **0,087 / 0,39 %** | 0,001 | 0,001 | 0,003 | 0,000 | **ok** |
| `plaingraph` mit Schatten | deferred | 0,153 / 0,40 % | 0,000 | 0,000 | 0,001 | 0,000 | ok |
| `builtin` mit Schatten | deferred | 0,174 / 0,43 % | 0,000 | 0,000 | 0,000 | 0,000 | ok |
| `builtin` mit Schatten | forward | 0,169 / 0,44 % | 0,000 | 0,000 | 0,000 | 0,000 | ok |
| `1` mit Schatten | forward | 2,30 / 4,38 % | 0,001 | **70,6 / 100 %** | 1,14 / 5,6 % | 0,000 | **nicht ok, §11.3** |
| `plaingraph` mit Schatten | forward | 3,47 / 4,41 % | 0,000 | **108,8 / 100 %** | 0,000 | 0,000 | **nicht ok, §11.3** |

- **Das Auto-Material ist auf Metal und GL gleich.** Masken, Normale, AO und Rauheit sind
  bitgleich. Das beleuchtete Bild liegt in jedem Pfad, in dem beide Backends denselben
  Schatten bekommen, weit innerhalb der Toleranz. Die Zahlen sind dieselben wie in §10.4.
- Die wenigen Pixel > 8 in den Zeilen mit 0,39–0,44 % liegen fast alle in den Spalten
  480–500. Dort sitzt die Schattenkante am Hangfuß, und der Halbschatten liegt um etwa
  1 px versetzt. Das eingebaute Material zeigt dasselbe Muster, mit dem Auto-Material hat
  es nichts zu tun.
- **Soll/Ist** (`ana158auto.py masks`): alle 15 Erwartungen auf Metal und GL erfüllt
  (Fels am Hang 100 %, Schnee auf dem Plateau 100 %, Wasser 18,3 % nur in der Ebene,
  Erdgürtel am Fuß 93,5 %).
- **Bombing** (`ana158auto.py repeat`): mit Bombing Oszillation 0,12 / 0,12 und kein
  periodisches Minimum; ohne 1,58 / 1,45 mit Minima bei 6, 12, 19, 25, 31, 37 px. Metal
  und GL liefern dieselben Zahlen.

### 11.3 Der Schatten am Hangfuß: es fehlt der GL-Forward-Graph-Schatten, Metal ist richtig

Bei TOD 0,4 steht die Sonne bei +x, in der x/y-Ebene 54° hoch (RenderExtractor:
`a = (tod − 0,25)·2π`, Richtung `(cos a, sin a, 0,45)`). Das Terrain ist entlang Z
konstant. Die Rampe wird bis 62° steil, also steiler als die Sonne. Der konkave Fuß
darunter zeigt zur Sonne, liegt aber im **Schlagschatten der steileren Rampe über
ihm**. Ein Strahl-Marsch über h(x) ergibt x = −24,5 … −16,3 (Spalten 487…532). Danach ist
die Rampe bis Spalte 643 von der Sonne abgewandt, und darin sind sich alle einig.

Neues Orakel `ana158auto.py shadow ON.bmp OFF.bmp …`: Es vergleicht je Backend die
Aufnahme mit Schatten gegen dieselbe mit `HE_DUMP_SHADOW=0.1` innerhalb der Zone (1 m
Abstand zu jeder Kante). Dafür braucht es kein Referenzbild von einem anderen Backend.

| Pfad | `1` | `nobomb` | `plaingraph` | `builtin` |
|---|---|---|---|---|
| Metal forward | 0,37 da | 0,37 da | 0,56 da | 0,47 da |
| Metal deferred | 0,37 da | | 0,56 da | 0,47 da |
| OpenGL deferred | 0,37 da | | 0,56 da | 0,47 da |
| OpenGL forward | **1,00 fehlt** | **1,00 fehlt** | **1,00 fehlt** | 0,47 da |

(Helligkeit in der Zone mit Schatten ÷ ohne Schatten; Ebene und Plateau ändern sich in
keinem Paar, Drift 0,000.)

- **Ursache:** `OpenGLRenderer.cpp:3577` / `:7665` / `:11671`. Im Forward-Pfad hält GL für
  Graph-Materialien absichtlich `csmSplits.w = 0`. `heCsm` liegt dort auf demselben
  Texturslot wie der Local-Shadow-Atlas, und `heCsmShadow` gibt sofort 1,0 zurück. Nur der
  Deferred-Resolve bekommt echte CSM-Matrizen. D3D11 (`D3D11Renderer.cpp:5979`), D3D12
  (`:9475`), Vulkan (`VulkanRenderer.cpp:6583`) und Metal (`MetalRenderer.mm:14116`)
  füllen die Kaskaden auch im Forward-Pfad für Graph-Materialien.
- **Folge:** Jedes Graph-Material auf OpenGL im Forward-Pfad empfängt keinen
  Sonnenschatten, nicht nur dieses. Das liegt nicht im Auto-Material, also gehört die
  Behebung nicht in diesen Schritt. Sie braucht ein eigenes Thema (Textur-Units von
  GL 4.1 gegen CSM + Local-Atlas abwägen). Die Deutung aus §10.4 („Metals heLitP“) war
  falsch herum.
- **Für den Bildvergleich heißt das:** Mit Schatten ist OpenGL-forward **keine**
  Referenz. D3D/Vulkan (forward-only, Thema 150) mit Schatten werden gegen Metal bzw. das
  Orakel verglichen, gegen GL nur mit `HE_DUMP_SHADOW=0.1`.

### 11.4 Rezept für D3D11 / D3D12 / Vulkan (NN-WS03)

1. Zweig auschecken. Release-`HorizonEditor` mit `-j8` bzw. `/m:8` bauen, den Deploy
   **inklusive `EditorDeps/EngineContent`** erneuern. Modus `1` lädt
   `Engine/Materials/M_AutoLandscape.hasset` per Pfad, alter Content gibt also ein
   falsches Bild. Prüfen, dass das Binary den Zeugen enthält (`findstr "AUTOLAND witness"`).
2. `cap158auto.ps1 -Backends OpenGL,D3D11,D3D12,Vulkan` für `masks ground normal surface`
   ausführen. Danach `1 nobomb plaingraph builtin`, jeweils einmal ohne Zusatz und einmal
   mit `-Extra @{HE_DUMP_SHADOW='0.1'} -Tag _s01`.
3. `ana158auto.py masks` (15 Erwartungen je Backend). `diff` gegen OpenGL für die
   Debug-Ansichten und die `_s01`-Läufe (Toleranz §7.5). `shadow` je Backend für die Paare
   `AL<m>-<rhi>.bmp AL<m>-<rhi>_s01.bmp`; erwartet: D3D11/D3D12/Vulkan „da“, OpenGL-forward
   bei Graph-Materialien „fehlt“. `repeat` für `1_s01` gegen `nobomb_s01`.
   **Korrektur aus Schritt 7 (§12):** Die Verhältnisse auf D3D/Vulkan sind NICHT die von
   Metal (0,37 / 0,56), solange Graph-Materialien dort kein Himmels-IBL bekommen; gemessen
   0,30 / 0,48. `-Extra` funktioniert nur, wenn das Skript im selben Prozess mit `&`
   aufgerufen wird (`powershell -File` macht aus der Hashtable einen String).
4. Vulkan-Validation-Zeilen aus den Logs mitmelden.
5. **Mips (Lektion aus Schritt 3):** Die drei Arrays backen ihre Mips selbst
   (`buildTextureArray`). D3D/Vulkan dürften in der Ebene also nicht flimmern. Falls doch,
   zuerst die Mip-Kette prüfen, nicht den Codegen.

### 11.5 Was offen bleibt

- **D3D11, D3D12, Vulkan gerendert:** Folgeschritt auf NN-WS03 (§11.4). Bis dahin ist
  das Akzeptanzkriterium „auf allen fünf Backends dasselbe Bild“ nur für Metal und
  OpenGL belegt, für die anderen drei nur per Cross-Compile.
- **lavapipe:** erst nach dem Merge von main in den Zweig (dann `he_vk_imagetests.py` um
  einen AUTOLAND-Fall erweitern; Software-Treiber, keine echte Hardware).
- **GL-Forward-Graph-Materialien ohne Sonnenschatten** (§11.3): eigenes Thema.
- **Echte Texturen** (§4.3, 25 PNG) und **Wetter-Kopplung** (Schnee/Pfützen aus dem
  Wetter): gehören laut Thema nicht hierher. Die Vorgaben der 14 Parameter werden mit
  den echten Texturen am Bild nachgestellt.
- **Handbuch:** Schritt 5 hat keine neuen Knoten gebracht. Die Knoten aus Schritt 3/4
  haben Tooltips im Editor (`EditorHelp.cpp`). Die Node-Library-Tabelle der Website
  (`HorizonEngineDocs/materials.html`, Zeile „Texture“) nennt nur Texture Sample und
  Panner. Sie bekommt Texture/Normal Map Array und die vier Bombing-Knoten **nach dem
  Merge**, damit die öffentliche Seite keine unveröffentlichten Knoten zeigt. Danach
  das Handbuch-Bündel `EditorDeps/Docs/he-docs.json` neu bauen.
- Die übrigen Punkte aus §10.5 gelten weiter (Pfützen in echten Mulden, heTex0 auf
  Metal, steile Felswände, malbare Overrides, GI-Näherung).

## 12. Schritt 7: D3D11, D3D12 und Vulkan auf echter Hardware

Stand: Zweig bei `55a93502` + `fa30d4b9`. Gemessen auf NN-WS03 (NVIDIA RTX 4070, Windows 11),
Release-Build `C:\hw158` mit eigenem Deploy. Aufnahmen mit `cap158auto.ps1` (gleiche
Kamera und Einstellungen wie §10.4/§11.2), Auswertung mit `ana158auto.py`. Metal gibt es
auf diesem Gerät nicht. Die Brücke zu Metal ist OpenGL: §11.2 hat Metal = GL auf dem Mac
gezeigt, und GL auf NN-WS03 trifft dieselben Zahlen (unten).

### 12.1 Absturz im Codegen: Stack-Überlauf unter MSVC (behoben, `fa30d4b9`)

Der erste Lauf scheiterte, bevor ein Bild entstand. Der Editor stürzte mit dem
Auto-Material zufällig ab, auf **allen** Backends einschließlich GL: `masks` 4/4 Läufe,
`ground` 1/4. Ursache war `0xC00000FD` (Stack Overflow) in `HorizonCore.dll`, in
`__chkstk`.

- **Ursache:** `emitNode` (MaterialGraph.cpp) hielt den ganzen Knoten-Switch in einer
  Funktion. MSVC reserviert dafür einen Frame für die Temporaries aller Fälle. Das ergab
  26,7 KB pro Graph-Ebene im Release (main 21,9 KB, Debug 42,8 KB). `emitNode` rekursiert
  einmal pro Ebene (`emitNode` → `inputExpr` → `emitNode`). Die Ansichten des
  151-Knoten-Graphen brauchten deshalb 836–964 KB Stack, und der Hauptthread hat 1 MB.
  Unter clang/macOS war der Frame klein und der Hauptstack 8 MB, darum blieb der Fehler
  auf dem Mac unsichtbar.
- **Gegenprobe ohne Neubau:** Eine Kopie des Deploys mit `editbin /STACK:8388608` lief
  5/5 durch.
- **Behebung:** Die Fälle stehen jetzt **wörtlich verschoben** in zehn
  `HE_MG_NOINLINE`-Familien: Blätter, Texturen, Bombing, Layer-Blend, Mathe, Logik,
  Muster, Fluss, UI, WindSway. Pro Ebene liegt nur der Frame der eigenen Familie auf dem
  Stack: `emitNode` 632 B, Familie ≤ 1416 B, `inputExpr` 192 B. Gemessen (Thread mit
  vorgegebenem Stack, SEH):

| Graph | vorher | nachher |
|---|---|---|
| Auto-Ansichten lit / nobomb / masks / ground / normal / surface | 836–964 KB | ≤ 68 KB (Messauflösung) |
| Kette aus 256 Add-Knoten | 6788 KB | 388 KB |
| Kette aus 1024 Add-Knoten | 27016 KB | 1476 KB |
| pro Ebene | 26,3 KB | 1,4 KB |

- **Bytegleich:** Der Shader-Text (glsl, glslGBuffer, vertexBody, Texturen, Parameter) hat
  vorher und nachher denselben Hash. Geprüft wurden alle sechs Auto-Ansichten, alle 76
  Knotentypen einzeln und die drei Ketten. Die Bilder aus §11 bleiben damit gültig.
- **Test** `Material codegen fits a small thread stack`: Er erzeugt die Auto-Ansichten
  auf einem Thread mit 512 KB und die 256er-Kette auf 4 MB. Mit der alten
  `HorizonCore.dll` schlägt er fehl, mit der neuen ist er grün.
  `test_material_graph` + `test_gltf_material_import`: 118/118.
- **Grenze:** Beliebig tiefe Nutzer-Graphen können auch jetzt überlaufen, aber erst bei
  rund 700 Ebenen statt bei rund 38. Neue Knoten gehören in eine passende Familie, nicht
  zurück in `emitNode`.

Nach der Behebung: 40 Aufnahmen auf vier Backends, 0 Abstürze.

### 12.2 Material (unlit)

Masken-Orakel (`ana158auto.py masks`): **15/15 Erwartungen auf GL, D3D11, D3D12 und
Vulkan.** Anteil Pixel > 128:

| Region / Kanal | Mac (Metal = GL) | GL | D3D11 | D3D12 = Vulkan |
|---|---|---|---|---|
| Ebene Wasser | 18,3 % | 18,3 % | 18,3 % | 17,9 % |
| Ebene Erde / nass | 33,2 / 30,6 % | 33,2 / 30,6 % | 33,2 / 30,6 % | 32,8 / 30,1 % |
| Fuß Fels / Erde | 27 / 93,5 % | 27 / 93,5 % | 27 / 93,5 % | 27 / 93,5 % |
| Hang Fels | 100 % | 100 % | 100 % | 100 % |
| Plateau Schnee / Erde | 100 / 2,8 % | 100 / 2,8 % | 100 / 2,8 % | 100 / 4,6 % |

Bildvergleich gegen GL (mean|Δ| / Anteil > 8):

| Ansicht | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| `masks` | 2,79 / 0 % | 5,33 / 4,57 % | 5,33 / 4,57 % |
| `ground` | 3,10 / 0 % | 9,22 / 12,2 % | 9,21 / 12,2 % |
| `normal` | 0,001 / 0 % | 0,114 / 0,05 % | 0,113 / 0,05 % |
| `surface` | 1,33 / 0 % | 3,41 / 7,05 % | 3,41 / 7,05 % |

- **D3D11 = GL**, bis auf einen **Schwarzpegel**: D3D11, D3D12 und Vulkan geben bei
  unlit-Schwarz 4/255 statt 0 aus. Das ergibt die konstanten 2,67 in jeder Region. Die
  Ursache ist nicht untersucht, sie liegt nicht im Material.
- **D3D12 = Vulkan** (bitgleich zueinander), aber in der Ebene sind die Umrisse der
  Pfützen und Erdflecken **blockig, am Rauschgitter ausgerichtet und versetzt**. Fels und
  Plateau sind gleich, die Mittelwerte fast gleich. Wahrscheinliche Ursache, **nicht
  bewiesen**: `heHash21` (`fract(p * vec2(123.34, 456.21))`, dann `fract(p.x * p.y)`) ist
  bei großen Gitterkoordinaten in den hohen Fbm-Oktaven numerisch instabil. D3D12 übersetzt
  wie D3D11 mit FXC (ps_5_0), der Unterschied entsteht also im Treiber (z. B.
  FMA-Kontraktion). Entscheidung der Queen (Anfrage 21): Das Auto-Material bekommt einen
  robusten Hash, bestehende Noise-Materialien bleiben unverändert. Das ist Schritt 8.
  **Bewiesen und behoben in §13.**

### 12.3 Beleuchtet

Bildvergleich gegen GL mit Schatten-Distanz 0,1 m (`_s01`):

| Modus | D3D11 | D3D12 = Vulkan | davon Hang (Schattenseite) |
|---|---|---|---|
| `1` | 4,81 / 22,6 % | 8,09 / 27,1 % | 26,0 / 100 % |
| `nobomb` | 4,81 / 22,6 % | 8,02 / 27,1 % | 25,8 / 100 % |
| `plaingraph` | 5,05 / 15,1 % | 5,05 / 15,1 % | 38,2 / 100 % |
| `builtin` | 45,8 / 99,9 % | 45,8 / 99,9 % | 20,1 / 100 % |

- **Sonnenseite gleich:** `plaingraph` Ebene und Plateau 0,67 (≤ 1 pro Kanal).
- **Schattenseite dunkler:** Auf D3D11/D3D12/Vulkan bekommen Graph-Materialien **kein
  Himmels-IBL**. `heLight.fog.z` bleibt dort 0, GL und Metal setzen es auf 1, sobald der
  Himmels-Cubemap existiert (`MaterialShaderLibrary.cpp` heLitP: `ambDiff` aus
  `heSkyEnv`). Am Hang gemessen: GL 120/139/165, D3D/Vulkan 90/98/122. Das ist eine
  Backend-Lücke außerhalb dieses Themas; die Queen sammelt sie für Thema 150.
- `builtin` weicht auf D3D/Vulkan überall ab (auch auf der Sonnenseite). Außerhalb dieses
  Themas, nicht untersucht.

**Schatten-Orakel** (`ana158auto.py shadow`, mit ÷ ohne Schatten in der Schlagschattenzone):

| Pfad | `1` | `nobomb` | `plaingraph` | `builtin` |
|---|---|---|---|---|
| Metal (Mac, §11.3) | 0,37 | 0,37 | 0,56 | 0,47 |
| OpenGL deferred, NN-WS03 | 0,37 | | 0,56 | |
| OpenGL forward, NN-WS03 | 1,00 fehlt | 1,00 fehlt | 1,00 fehlt | 0,47 |
| D3D11 | 0,30 da | 0,30 da | 0,48 da | 0,50 da |
| D3D12 | 0,30 da | 0,30 da | 0,48 da | 0,50 da |
| Vulkan | 0,30 da | 0,30 da | 0,48 da | 0,50 da |

Ebene und Plateau ändern sich in keinem Paar (Drift 0,000). Der Schlagschatten ist auf
allen drei Backends da. Die kleineren Verhältnisse passen zum fehlenden IBL: Die Zone ist
nur ambient beleuchtet, mit Schatten 27,5 statt 36,1 (GL deferred).

### 12.4 Bombing und Validation

- `ana158auto.py repeat` (Ebene, `_s01`): mit Bombing Oszillation GL 0,12/0,12, D3D11
  0,12/0,12, D3D12/Vulkan 0,16/0,14, ohne periodisches Minimum. Ohne Bombing 1,57/1,44
  (GL), 1,59/1,46 (D3D11), 1,50/1,40 (D3D12/Vulkan) mit Minima bei 6, 12, 19, 25, 31,
  37 px. Die Wiederholung ist auf allen vier Backends gebrochen.
- Vulkan-Validation: **0 Meldungen** in jedem Modus mit Graph-Material. `builtin` zeigt 13
  VUIDs (`vkCmdUpdateBuffer` im Render-Pass, Barrier ohne Self-Dependency). Das ist der
  Fehler, den PR #96 auf main behoben hat; der Zweig hat main noch nicht.

### 12.5 Urteil

Die Toleranz aus §7.5 (mean|Δ| ≤ 1,0, ≤ 0,5 % Pixel > 8) ist auf D3D11/D3D12/Vulkan gegen
GL **nicht erfüllt**. Die Ursachen sind benannt:

1. **Im Thema:** Noise-Hash auf D3D12/Vulkan → Schritt 8 (robuster Hash nur für das
   Auto-Material, dann D3D12/Vulkan gegen D3D11 messen).
2. **Außerhalb:** kein Himmels-IBL für Graph-Materialien auf D3D11/D3D12/Vulkan (Thema
   150), Schwarzpegel 4/255, `builtin`-Abweichung, main-Merge (Vulkan-VUIDs bei `builtin`).

Erfüllt sind: Fels am Hang, Schnee in der Höhe, Pfützen nur auf flachem Boden, Erdgürtel
am Fuß (Orakel 15/15) und gebrochene Wiederholung, jeweils auf allen vier
Windows-Backends.

## 13. Schritt 8: Integer-Hash für das Auto-Material

Stand: Zweig bei `64aeabb5` + `bec72dba`. NN-WS03 (RTX 4070), Release `C:\hw158`, frischer
Deploy samt `EngineContent`, gleiche Kamera und Skripte wie §12. Aufnahmen in
`C:\hw158\shots8` (lokal).

### 13.1 Ursache und Behebung

`heHash21` multipliziert den Gitterindex vor dem `fract()` mit 123,34 bzw. 456,21. Das
Pfützenfeld liest `(xz + (173,1; 419,7)) / Puddle Size`. In der vierten Oktave (× 8)
liegt der Index damit bei rund 560, das Produkt bei rund 2,5·10⁵. Bei dieser Größe
bleiben einem float nur noch etwa 6 Nachkommabits. Jede FMA-Kontraktion oder
Umordnung im Treiber ergibt dann pro Zelle einen anderen Wert. Das waren die blockigen,
am Rauschgitter ausgerichteten Umrisse auf D3D12/Vulkan aus §12.2. Die Vermutung von
dort ist damit **bewiesen**: Mit dem Integer-Hash sind sie weg (13.3).

- **Fbm-Knoten, `p[0]` = Hash-Variante.** `0` ist die Vorgabe und schreibt weiter
  `heFbm`/`heHash21` mit demselben Helfer-Block. `1` schreibt `heFbmI`: pcg2d
  (Jarzynski & Olano 2020) auf der ganzzahligen Zelle, float → int → uint, exakte
  24-Bit-Umwandlung. Das ist dasselbe Muster wie `heBombHash`, das seit Schritt 4 auf
  allen fünf Backends läuft. Interpolation und Oktaven sind unverändert.
- **Nur das Auto-Material setzt `p[0] = 1`**, auf beiden Fbm-Feldern (Erdflecken,
  Pfützenbecken). `M_AutoLandscape.hasset` ist mit `landscape_tex_gen --material` neu
  erzeugt (Drift-Test grün).
- Das Flag hat **keinen Editor-Regler** (Fbm bleibt `paramCount 0`, damit sich kein
  bestehender Knoten und keine MCP-Ausgabe ändert). Es steht im JSON (`"p"` wird immer
  ganz geschrieben) und überlebt Speichern und Laden (Test). Kopieren/Einfügen und Undo
  im Editor laufen über dasselbe Graph-JSON, MCP ändert Knoten an Ort und Stelle. Nur die
  Lese-Ausgabe von MCP (`material_graph_info`) kürzt `p` auf `paramCount` und zeigt
  das Flag deshalb nicht.
- Die Kosten-Anzeige im Material-Editor zählt `heFbmI(` mit.

### 13.2 Bestehende Materialien unverändert

- **Probe aus Schritt 7** (Hash von glsl, glslGBuffer, vertexBody, Texturen, Parameter),
  alte gegen neue `HorizonCore.dll`: alle **76 Knotentypen** (`788e6c1397262ed2`) und die
  drei Ketten 64/256/1024 sind **bytegleich**. Neu sind nur die sechs Auto-Ansichten.
- **Content:** Im ganzen Repo (`EditorDeps`, `src`, `tests`, `scripts`, `docs`) hat nur
  `M_AutoLandscape.hasset` Fbm-Knoten. Vorher stand dort `p[0] = 0`. Ein anderes Asset
  mit gesetztem `p[0]` gibt es nicht. Über die Oberfläche und MCP lässt sich `p[0]` beim
  Fbm nicht setzen (`paramCount 0`).
- **Test** `Fbm p[0] = 1 hashes on integers; the default keeps the float hash`: Vorgabe =
  `heHash21`/`heFbm(`, ohne `heFbmI`. Flag = `heFbmI(`, ohne `heHash21`. JSON-Rundreise
  bytegleich. Alle fünf Auto-Ansichten nur mit `heFbmI`.
  `test_material_graph` + `test_gltf_material_import`: **119/119**. Dazu gehören die
  FXC-, SPIR-V- und GL-Link-Sweeps mit dem Fall „Auto landscape material“.

### 13.3 Messung: D3D12/Vulkan gegen D3D11

Der neue Hash ergibt eine neue Verteilung von Erde und Pfützen, deshalb ist auch die
D3D11-Referenz neu aufgenommen (alle vier Backends, 30 Aufnahmen, 0 Abstürze, 0
Vulkan-Validation-Meldungen). Vergleich jeweils gegen **D3D11** (gleicher Schwarzpegel),
mean|Δ| / Anteil > 8:

| Ansicht | D3D12 | Vulkan | vorher D3D12 gegen D3D11, Ebene (`shots7`) |
|---|---|---|---|
| `masks` | 0,000 / 0 % | 0,000 / 0 % (Fuß 0,004) | |
| `ground` | 0,000 / 0 % | 0,000 / 0 % | **22,6 / 41 %** |
| `normal` | 0,000 / 0 % | 0,000 / 0 % (Hang 0,005) | |
| `surface` | 0,000 / 0 % | 0,000 / 0 % | |
| `1` / `nobomb` | 0,000 / 0 % | ≤ 0,002 / 0 % | |
| `1_s01` / `nobomb_s01` | 0,000 / 0 % | ≤ 0,005 / 0 % | |

**D3D12 und Vulkan sind jetzt bildgleich zu D3D11**, unlit wie beleuchtet. Die Toleranz
aus §7.5 (≤ 1,0 / ≤ 0,5 %) ist zwischen den drei Backends erfüllt, mit großem Abstand.

- **Masken-Orakel:** 8/8 Aufnahmen (`masks`, `ground` × GL/D3D11/D3D12/Vulkan) erfüllen
  alle Erwartungen. Ebene: Wasser 12,8 %, Erde 20,8 %, nass 22,9 % (vorher 18,3 / 33,2 /
  30,6 %, das andere Rauschen). Fuß Erde 93,9 %, Hang Fels 100 %, Plateau Schnee 100 %.
- **Gegen OpenGL, unlit:** nur noch der Schwarzpegel. `masks` 2,80 / 0 %, `ground`
  3,20 / 0 %, `surface` 1,33 / 0 %, `normal` 0,001 / 0 %. In allen Regionen ist der Anteil
  > 8 gleich 0 %. Die Werte liegen also im Rahmen der konstanten 4/255.
- **Gegen OpenGL, beleuchtet (`1_s01`):** 4,75 / 20,9 %, also wie D3D11 in Schritt 7
  (4,81 / 22,6 %). Hang 26,0 (Schattenseite, kein Himmels-IBL, §12.3). Ebene 3,95: D3D11
  ist dort gleichmäßig dunkler, Gras/Erde um 3,4, nasser Boden um 6,0 (GL 66,1 gegen
  60,1). Das passt zum fehlenden Himmels-IBL (Umgebungslicht, und Spiegelung auf dem
  glatten Wasser), ist hier aber **nicht bewiesen**. Es liegt nicht am Hash: D3D11, D3D12
  und Vulkan sind darin bitgleich, und D3D11 zeigte es schon vorher.
- **Schatten-Orakel:** D3D11/D3D12/Vulkan 0,30, Schatten da. GL deferred 0,37 (= Metal,
  §11.3), GL forward 1,00, Schatten fehlt (§11.3). Gleich wie in §12.3.
- **Wiederholung** (`repeat`, `_s01`): Mit Bombing beträgt die Oszillation auf allen vier
  Backends 0,12 / 0,08, ohne Bombing 1,68–1,70 / 1,51–1,53 mit Minima alle 6 px. Die
  Wiederholung ist überall gleich gebrochen.

### 13.4 Metal

Auf NN-WS03 gibt es kein Metal. Das Bild „Metal = GL“ aus §11.2 galt für den **alten**
Hash. Für `heFbmI` ist Metal nur durch den Cross-Compile in der macOS-CI belegt. Das
Hash-Muster (uint-Arithmetik, `>>`, float(uint)) ist dasselbe wie bei `heBombHash`, das
in §11.2 auf Metal gerendert wurde (Modus `1` mit Bombing, gegen GL 0,001 / 0 %). Ein neuer Metal-Lauf
(`cap158auto.sh … masks ground`, dann `ana158auto.py diff` gegen GL) steht noch aus.

### 13.5 Was offen bleibt

- Metal-Aufnahme mit `heFbmI` (13.4), auf einem Mac.
- Außerhalb dieses Themas, unverändert aus §12.5: Himmels-IBL für Graph-Materialien auf
  D3D11/D3D12/Vulkan (Thema 150, erklärt den beleuchteten Rest gegen GL), Schwarzpegel
  4/255, `builtin`-Abweichung, main-Merge.
- Bestehende Noise-Materialien behalten bewusst `heHash21`. Wer in Weltkoordinaten oder
  mit großem Scale rauscht, sollte beim Fbm `p[0] = 1` setzen. Ein Editor-Regler dafür
  wäre ein eigener kleiner Schritt (paramCount 1 ändert jeden Fbm-Knoten in UI und MCP).

## 14. Die echten Texturen: importiert und zu Arrays gebaut (Thema 177, Schritt 1)

Stand: Zweig `claude/auto-landscape-material-texturen-importieren-arrays-bauen-ve`,
MacBook Air (Apple M5), Release-Build von `landscape_tex_gen`. Es ist **kein Engine-Code**
geändert. Neu sind der Pack-Modus (`1f99f8fe` von `release/0.7.0`, hier per cherry-pick),
zwei Skripte unter `scripts/landscape-textures/` und eine Zeile in `.gitignore`.

### 14.1 Was der Mensch geliefert hat

Ordner `EditorDeps/Images/Landscape/` im Haupt-Checkout, **nicht** in git (jetzt
ignoriert). Alle 16 Dateien sind **byteidentisch zu den Poly-Haven-Originalen** (Größe und
MD5 gegen `api.polyhaven.com/files/<id>` geprüft). Poly Haven ist CC0.

| Schicht | Poly-Haven-Asset | Autor | Dateien (4K) |
|---|---|---|---|
| Grass | `grass_ground` | Charlotte Baglioni | `Grass_Color.jpg` (= `grass_ground_diff_4k.jpg`, nur umbenannt), `_nor_gl` EXR, `_rough` EXR, `_disp` PNG 16 Bit |
| Dirt | `dirt` | Charlotte Baglioni | `dirt_diff` JPG, `_nor_gl` EXR, `_rough` EXR, `_disp` PNG 16 Bit |
| Rock | `rocks_ground_08` | Rob Tuytel | `_diff` JPG, `_nor_gl` EXR, `_rough` JPG, `_disp` PNG 8 Bit |
| Snow | `snow_02` | Rob Tuytel | `_diff` JPG, `_nor_gl` EXR, `_rough` JPG, `_disp` PNG 8 Bit, `_translucent` (ungenutzt) |
| WetGround | **fehlt** | | |

- **WetGround fehlt.** Entscheidung der Queen: Platzhalter, bis der Mensch das Set nachliefert.
  Die Schicht zeigt dann das hochskalierte Schachbrett mit L-Marke. Im Auto-Material
  taucht es am nassen Rand der Pfützen auf. Das ist ein Platzhalter, kein Fehler.
  **Überholt durch Thema 177, Schritt 3 (§16):** Das Auto-Material liest die Schicht nicht
  mehr, die Pfützen sind ein Overlay. Das Set wird nicht mehr gebraucht.
- **AO fehlt bei allen vier.** Poly Haven bietet AO als eigene Datei an (`_ao_`, oder `_arm_`
  mit AO in R und Rauheit in G). Das Skript nimmt sie automatisch mit, sobald sie im
  Schichtordner liegen. Ohne AO ist Masken-R weiß, also kein AO-Anteil.
- Die Dateien sind 4K, die Doku verlangt 2K (§4.1). Die Normal-Maps sind **EXR mit
  DWAA-Kompression**. stb_image (der Importer, `--pack`) liest kein EXR, `sips` auch nicht.

### 14.2 Zwischenschritt `stage_polyhaven.py`

`scripts/landscape-textures/stage_polyhaven.py <Quellordner> <Staging> --size 2048` macht
aus den Poly-Haven-Ordnern den flachen Satz `<Schicht>_<Map>.png`, den `--pack` liest:

- erkennt die Maps am Namen (`diff`/`color`, `nor_gl`, `rough`, `ao`/`arm`, `disp`), lässt
  `nor_dx` und alles Unbekannte liegen und meldet es,
- liest EXR über das Python-Modul OpenEXR (`pip install numpy Pillow OpenEXR` in einer
  Wegwerf-venv, Aufruf mit `python3 -I`),
- 4K → 2K durch exaktes 2×2-Mitteln in float,
- **Normal-Maps nach dem Verkleinern neu normiert** (Mittel der Längen vorher 0,87 bis
  1,02, danach 1), kein Gamma: 0..1-Kodierung bleibt linear,
- **Höhe über die Kachel auf 0..1 gestreckt** (0,5 %/99,5 %-Perzentil, §4.2). Rohbereiche
  waren zum Beispiel 0,17..0,58 (Grass) und 0,32..0,92 (Rock). Ohne das hätte die
  Höhen-Überblendung (§10.2) kaum Spielraum,
- spiegelt **nichts**: der Importer dreht beim Laden, die Dateien bleiben wie geladen,
- schreibt `stage_manifest.txt` (welche Quelldatei zu welcher Map wurde).

### 14.3 Packen und Ergebnis

```
python3 -I scripts/landscape-textures/stage_polyhaven.py EditorDeps/Images/Landscape out/landscape-real/staging --size 2048
landscape_tex_gen out/landscape-real/Engine/Textures/Landscape --pack out/landscape-real/staging --size 2048
```

Ausgabe `out/landscape-real/Engine/Textures/Landscape/` (560 MiB, **nicht in git**): 15
Einzeltexturen je 16 MiB (RGBA8, nur Mip 0) und die drei Arrays je 107 MiB (5 Slices in der
Reihenfolge Grass, Dirt, Rock, Snow, WetGround, **12 Mips**, die Kette gebacken). Feste
UUIDs `0x400..0x411`, also bleiben alle Verweise heil. `--pack` meldete 15/15 und 3/3
geschrieben, jede Datei liest es im selben Lauf zurück.

Unabhängige Gegenprobe mit `scripts/landscape-textures/hasset_tex.py` (liest .hasset in
Python, kennt nur das Dateiformat):

| Prüfung | Ergebnis |
|---|---|
| Arrays | 2048², 5 Slices, 12 Mips, Albedo sRGB, Normal und Maske linear, UUIDs 0x40F/0x410/0x411 |
| Albedo-Array Slice 0 (Grass), 3 (Snow), Normal-Array Slice 2 (Rock) gegen die gestagten PNGs | max. Abweichung **0** in allen Kanälen |
| Masken-Array Slice 1 (Dirt): G gegen `Dirt_Roughness`, B gegen `Dirt_Height` | max. Abweichung **0** |
| Negativkontrolle: Masken-R gegen `Dirt_Roughness` | max. 29, Mittel 14,2 (R ist weiß, Rauheit liegt bei 0,94: der Vergleich unterscheidet also) |
| Sichtprüfung aller 15 Slices (Mip 3) | Normal-Maps sind blau-violett (+Z), Slice 4 ist das Platzhalter-Schachbrett, die übrigen vier zeigen die echten Oberflächen |
| `landscape_tex_gen <Ordner>` ohne Flags (Platzhalter), nach dem cherry-pick | alle 18 Dateien **byteidentisch** zu `EditorDeps/EngineContent/Textures/Landscape/`: der Pack-Modus berührt den alten Pfad nicht |
| Rauheit als JPG (Rock, Snow) und als EXR (Grass, Dirt) gleich behandelt: Sind die 8-Bit-JPGs linear? | Ja. `dirt_rough_4k.jpg` (neu von Poly Haven, MD5 passt) hat Mittel 240,8, die EXR derselbe Wert 240,8, mittlere Abweichung 0,0016. Mit sRGB-Kodierung läge es bei 248,6 |

Mit `--arrays-only` lassen sich die Arrays aus den 15 Einzeldateien ohne neues Packen
neu bauen (wenn zum Beispiel eine Schicht ersetzt wurde).

### 14.4 Befunde an den Quellen (nichts davon ist korrigiert)

- **Grass wirkt trocken, nicht grün.** `grass_ground` ist laut Tags trockenes Gras mit
  Wurzeln. Grass und Dirt liegen farblich nah beieinander (Mittel sRGB 110/96/62 gegen
  99/82/62). Ob die Auto-Verteilung dadurch weniger Kontrast zwischen Wiese und Erdflecken
  zeigt, ist erst am Bild zu sagen (Schritt Verifikation). Wer es grüner will, braucht ein
  anderes Set.
- **Schnee ist nicht 0,8.** `snow_02` hat im Mittel sRGB 165 (§4.3 wollte etwa 0,8, also
  sRGB 204). Er ist grau gegen reinen Schnee.
- **Rauheit von Dirt** liegt nur zwischen 0,88 und 0,99, fast konstant.
- Die Höhe von Rock und Snow liegt als **8-Bit-PNG** vor (Grass und Dirt als 16 Bit). Das
  zeigt sich als Stufen in der Höhen-Überblendung. Wer es schöner will, nimmt die
  16-Bit-Variante oder die EXR-Displacement-Datei.
- Die Normal-Map von Snow hat einen mittleren Hang (Mittel R/G 0,56 statt 0,5): eine leichte
  Vorzugsneigung, die auf der Fläche als einheitlicher Lichtton sichtbar werden kann.
- Der 2K-Satz ist **unkomprimiert**: Die drei Arrays belegen zusammen etwa 320 MiB im
  Speicher und auf der Grafikkarte (§5, BC7 für Arrays fehlt weiter, §8.5).

### 14.5 Benutzen

Beide Wege folgen §4.4 und §6. Ein Editor- oder Render-Lauf mit den echten Texturen war in
diesem Schritt **nicht** dabei, sie sind hier also nicht ausprobiert.

- Die Ausgabe ist ein fertiger **Projekt-Override-Ordner**: den Inhalt von
  `out/landscape-real/Engine/` nach `<Projekt>/Content/Engine/` kopieren, dann schlägt er
  die eingecheckten Platzhalter (§4.4).
- Für einen Editor-Lauf aus dem Build-Baum: die 18 Dateien über
  `out/deploy/Editor/EngineContent/Textures/Landscape/` kopieren. **Nach jedem Neulinken
  von `HorizonEditor` neu kopieren**, denn dessen POST_BUILD kopiert ganz `EditorDeps/`
  zurück und stellt damit die Platzhalter wieder her.
- Wohin die echten Texturen **dauerhaft** gehören (SFTP-Veröffentlichung, Projekt-Override
  oder LFS, §4.4) ist weiter offen und Sache des Menschen. Bis dahin nichts davon
  committen. `EditorDeps/Images/Landscape/` ist deshalb in `.gitignore`.
- **Fallstrick beim Quellordner:** `.gitignore` hält die Rohdateien (rund 400 MB) nur aus
  git, nicht aus dem Build. Der POST_BUILD von `HorizonEditor` kopiert `EditorDeps/`
  **ganz** (`copy_directory`, `src/HE_Editor/CMakeLists.txt:457-462`) neben die exe und in
  den Deploy, und `scripts/package_macos.sh:179-181` kopiert `EditorDeps/Images` in die
  `.app`. Die Quellen landen so in jedem Editor-Deploy und im DMG. Sie gehören langfristig
  außerhalb von `EditorDeps/` (das Staging-Skript nimmt jeden Ordner). Das gilt genauso für
  die Variante 2 in §4.4 (`…/Landscape/Source/`). Auf dem Haupt-Checkout (Zweig
  `release/0.7.0`) zeigt `git status` den Ordner bis zum Merge weiter als nicht verfolgt.

### 14.6 Was offen bleibt

- ~~**WetGround** nachliefern, dann `stage_polyhaven.py` und `--pack` neu laufen lassen.~~
  Entfällt: Pfützen sind seit Thema 177, Schritt 3 ein Overlay (§16).
- **AO** der vier Schichten nachladen (optional).
- **Bildprüfung** mit den echten Texturen auf den Backends, die Vorgaben aus §10.2
  (Steigungen, Pfützenmenge, Kachelgrößen) nachstellen, Triplanar-Frage für den Fels
  (§10.5). Gerendert ist in diesem Schritt nichts, geprüft ist nur der Inhalt der Dateien.
  **Nachtrag Thema 177, Schritt 2:** Metal und OpenGL sind gerendert, AO ist geklärt, siehe §15.
- Dauerhafter Speicherort und Kompression (BC7 für Arrays).

## 15. Verifikation mit den echten Texturen und die AO-Frage (Thema 177, Schritt 2)

Stand: Zweig `claude/auto-landscape-material-texturen-importieren-arrays-bauen-ve`,
MacBook Air (Apple M5), Release-Build von `HorizonEditor` (`out/build/macos-release`),
**Metal und OpenGL**. D3D11, D3D12 und Vulkan sind auf diesem Gerät nicht lauffähig und
**nicht gerendert** (§15.6). Es ist kein Engine-Code geändert. Bilder liegen unter
`docs/img/auto-landscape-echte-texturen-2026-10-08/`.

### 15.1 Aufbau

- **Texturen:** drei Varianten, jeweils die 18 `.hasset` aus §14 nach
  `out/deploy/Editor/EngineContent/Textures/Landscape/` kopiert (der Editor liest sie dort,
  §14.5): `ph` = die eingecheckten 128er-Platzhalter, `real` = Ausgabe von Schritt 1
  (Masken-R weiß), `realao` = dieselben Quellen **plus Poly-Haven-AO** (§15.3, 4K-JPG,
  MD5 gegen `api.polyhaven.com/files/<id>` geprüft, mit `stage_polyhaven.py` und
  `--pack` wie in §14.3 gepackt, **nur lokal, nicht in git**).
- **Zeuge** `HE_DUMP_AUTOLAND=1` (§10.4: das ausgelieferte Asset, 128-m-Relief auf y = 300),
  `scripts/auto-landscape-repro/cap158auto.sh`. Draufsicht wie in §10.4 (TOD 0,4, forward,
  GI/SSAO/AA aus). Dazu vier Schrägansichten von der Ebene (−x) nach +x, **deferred**, TOD
  0,6 (Sonne hinter der Kamera): `wide` (−62, 314, Nick −12°), `plain` (−50, 301,8, −30°),
  `rock` (−19, 313,5, −8°), `snow` (14, 341,8, −30°); alle Yaw 90°.
- Keine `[ERROR]`-, Link- oder Compile-Zeile in einem der 43 Läufe (alle Logs durchsucht).
  Draw-Counter: 4 Draws (forward), 5 (deferred), wie in §10.4.
- Kleinigkeit am Skript: `cap158auto.sh` verlangte einen **absoluten** Ausgabeordner (jede
  Aufnahme läuft nach einem `cd` in den Deploy-Ordner, ein relativer Pfad legte nur eine leere
  Logdatei an und meldete `bmp=NO`). Jetzt wird der Pfad aufgelöst.

### 15.2 Das Bild

Metal gegen OpenGL mit den echten Texturen (`real`), Differenz pro Pixel (Mittel / Anteil
> 8 von 255):

| Ansicht (deferred) | Metal gegen GL |
|---|---|
| `wide` / `plain` / `rock` / `snow` | 0,004 / 0,011 / 0,010 / 0,002 (Anteil > 8: 0,000 / 0,004 / 0,000 / 0,000 %) |
| Draufsicht TOD 0,4, Spalten Ebene / Hangfuß / Fels / Plateau | 0,001 / 0,001 / 0,004 / 0,000 |

Die echten Texturen laufen im Auto-Material **auf beiden Backends gleich**; die Normal-Maps
stehen richtig herum (die Beulen lesen sich konvex, und die Normal-Maps der vier Sets
korrelieren mit dem Gradienten ihrer Höhenkarte so, wie es die GL-Konvention verlangt:
n.x gegen dh/dSpalte −0,57 bis −0,86, n.y gegen dh/dZeile +0,56 bis +0,84, jeweils Grass,
Dirt, Rock, Snow). Forward mit Schatten unterscheidet sich weiter am Hangfuß
(GL-Forward zeichnet bei Graph-Materialien keinen Sonnenschatten, §11.3); das ist nicht neu.

Was die Bilder zeigen (`echte-texturen-vier-ansichten.png`, `draufsicht-platzhalter-echt-ao.png`):

- **Grass und Erde sind gut zu unterscheiden** (olivgelb gegen Braun, auch in der Weitsicht).
  Die Sorge aus §14.4 trifft nicht zu. Das Gras ist trocken, nicht grün; das ist das Set.
- **Bombing wirkt mit den echten Texturen** (`ana158auto.py repeat`, Ebene: Oszillation
  0,11 horizontal / 0,06 vertikal, kein periodisches Minimum außer dem einen bei 37 px, das auch
  die Platzhalter haben).
- **Schnee kachelt sichtbar.** Das Set `snow_02` hat markante dunkle Spuren, und Schnee wird
  nicht gebombt (§9.4, `snow`-Seed −1 in `AutoLandscapeMaterial.cpp`). Draufsicht, Plateau,
  Verschiebungs-Differenz D(k): Minima genau bei **42 px und 83 px**, das sind 4 m und 8 m
  (4 m ≙ 41,6 px bei 10,39 px/m), also die `Rock Tile Size`. Im Bild eine Gitterstruktur aus
  gleichen Spuren (auch in `draufsicht-…png` rechts). Behebung wäre ein Seed für Schnee (6 Zugriffe
  mehr, 33 → 39); das ist eine Änderung am Builder, am generierten Asset und am Wächtertest,
  nicht Teil dieses Schritts.
- **Fels am Steilhang streckt sich.** `rocks_ground_08` ist ein warmes, sandfarbenes Geröll,
  kein graues Kliff. Bei 55–62° wird es über Welt-XZ ≈ 2-fach in Hangrichtung gezogen
  (1/cos 62° = 2,1) und liest sich in der Weitsicht als senkrecht gestreifte Wand (`wide`),
  ähnlich wie Stroh. Aus der Nähe (`rock`) wirkt es als Gestein. Das ist die Frage aus
  §10.5 (Triplanar/Biplanar, 2 bis 3-mal so viele Zugriffe); jetzt mit Bild belegt, noch nicht
  entschieden.
- **Pfützen** zeigen am Rand das **WetGround-Platzhalter-Schachbrett** (L-Marken), wie
  erwartet (§14.1). *Behoben in Schritt 3: Overlay statt Schicht, §16.* Die Wasserfarbe hängt stark vom Himmel (tief marineblau von oben, hellblau
  schräg).
- Schnee ist hell und grau (Mittel sRGB 165), nicht reinweiß (§14.4); im Bild kein Problem.

Die Vorgaben aus §10.2 (Steigungen 0,12 / 0,12, Pfützenmenge 0,32, Kachelgrößen 2 m / 4 m,
Dirt 0,35) wurden **nicht verändert**. Mit den echten Texturen sehen Verteilung und
Größenverhältnisse in den Bildern plausibel aus; eine Feinabstimmung am Geschmack ist offen.

### 15.3 AO: Quelle, Verwendung, Wirkung

**Quelle.** Für alle vier Sets bietet Poly Haven eine AO-Karte (`AO`, `arm`, 1K bis 8K, EXR/JPG/PNG).
Sie fehlte nur, weil sie nicht mitgeliefert wurde. `stage_polyhaven.py` nimmt `*_ao_*`
automatisch. AO der JPG ist linear wie die Rauheit (§14.3), kein Decode nötig.

| Schicht | AO-Mittel | 0,5 %-Perzentil | 99,5 %-Perzentil | Maximum |
|---|---|---|---|---|
| Grass | 0,80 | 0,46 | 0,97 | 1,00 |
| Dirt | 0,90 | 0,60 | 0,97 | 1,00 |
| Rock | 0,71 | 0,36 | 0,88 | 1,00 |
| Snow | **0,55** | 0,42 | **0,61** | **0,64** |

`snow_02` ist die Ausnahme: Seine AO liegt flächig bei 0,55 und erreicht nie weiß. Ungeprüft
übernommen würde sie jede Schneefläche im Indirekten um fast die Hälfte abdunkeln.

**Wie die Engine AO benutzt** (Code, nicht Bild): Masken-R geht an den AO-Pin des
Ausgabeknotens (`AutoLandscapeMaterial.cpp:243`). In `heLitP`
(`MaterialShaderLibrary.cpp:738-750`) wirkt sie als
`(ambDiff * 0,35 + ambSpec) * ao + Umgebungsboden * Diffus`, mit `ao = Material-AO * SSAO`.
Deferred rechnet denselben Faktor: Diffus im Resolve (`heLitP(…, g2.a)`, `:1283`), der
Spiegelanteil `ambSpec` im Reflexionspass (`:1775-1781`, gleiche Gate `giProbe.y`):

- sie dunkelt **nur das indirekte Licht** ab, **nie das direkte Sonnenlicht**,
- sie dunkelt den **Umgebungsboden** (`ambient`) nicht ab,
- **mit GI an** (`giProbe.y > 0,5`) wird sie **ganz übergangen**, die Sonden tragen die
  Verdeckung selbst. Im Editor ist GI standardmäßig **aus** (`EditorConfig.h:193`).

**Gemessen** (`real` gegen `realao`, Metal, Mittel |Δ| von 255 / Anteil > 8):

| Ansicht | GI aus | GI aus + SSAO an | GI an |
|---|---|---|---|
| Draufsicht TOD 0,4, **Fels** (liegt im Schatten, nur Indirektes) | **6,94 / 67,3 %** | 6,92 / 66,8 % | 0,004 / 0 % |
| Draufsicht, Ebene / Hangfuß / Plateau | 0,70 / 1,43 / 0,91 | 0,70 / 1,42 / 0,90 | 0,01 / 0,05 / 0,00 |
| Schräg `rock` (TOD 0,6, Fels sonnenbeschienen) | 2,21 / 0,68 % | 2,20 / 0,65 % | 0,004 / 0 % |
| Schräg `wide` | 1,42 / 0,02 % | 1,40 / 0,02 % | 0,016 / 0 % |
| Deferred, Draufsicht, ganzes Bild | 1,80 / 9,3 % (Forward 1,79 / 9,2 %) | | 0,006 / 0 % |

- AO ist **sichtbar nur dort, wo das Indirekte dominiert** (Schattenseite von Fels, bei
  niedriger Sonne), sonst um 1 bis 2 von 255 (`ao-wirkung-gi-aus-an.png`).
- **GI an: AO hat keine Wirkung** (Metal forward und deferred). Das entspricht dem Code.
- **SSAO** ändert auf diesem glatten Relief fast nichts (0,03 bis 0,08), die beiden Verdeckungen
  addieren sich hier also nicht merklich.
- **OpenGL:** `HE_DUMP_GI=1` liefert in diesem Zeugen ein **bitgleiches** Bild zu `GI=0` (forward und
  deferred). Dort war GI also nicht wirksam; die GI-Spalte gilt nur für Metal. Nicht weiter
  untersucht, hat mit AO nichts zu tun.

**Urteil.** AO lohnt sich, kostet nichts (kein neuer Sampler, Masken-R wird schon gelesen) und
macht die Schattenseiten von Fels und Gras ein wenig plastischer, **aber der Effekt ist klein und
verschwindet mit GI**. Für Grass, Dirt und Rock ist die Karte unverändert brauchbar. **Snow braucht eine
Normierung** (99,5 %-Perzentil auf 1 strecken, wie es `stage_polyhaven.py` mit der Höhe macht),
sonst wird der Schnee im Schatten zu dunkel. Das Skript tut das für AO **nicht**. Für die Albedo ändert
sich nichts: die Poly-Haven-Albedo ist delit (§4.1), AO wird nicht doppelt gezählt.
Wer AO nutzen will, lädt die vier `*_ao_4k.jpg` zu den Quellen (`https://api.polyhaven.com/files/<id>`,
Schlüssel `AO`), staged und packt neu (§14.3, `--arrays-only` genügt nicht, die Einzeltexturen
ändern sich).

### 15.4 Was bestätigt, was nicht

Bestätigt (Bild, Metal + OpenGL): die echten Texturen laden und kacheln im Auto-Material,
Verteilung (Fels am Hang, Schnee oben, Erdflecken, Pfützen) stimmt mit §10.4 überein, Metal und
OpenGL stimmen im Deferred-Pfad auf < 0,02 überein, Bombing wirkt, Normal-Maps stehen richtig herum.

**Nicht** geprüft: D3D11, D3D12, Vulkan (§15.6); Leistung und GPU-Speicher mit 2K-Arrays
(§14.4: ≈ 320 MiB, nicht gemessen); die Last der 33 Zugriffe; WetGround (Platzhalter);
Schnee unter Schatten oder Bewölkung; der Editor im Normalbetrieb (nur der Dump-Pfad).

### 15.5 Was offen bleibt

- **Schnee bomben** (Seed), sonst sichtbare 4-m-Kachelung (§15.2).
- **Fels am Steilhang**: Triplanar/Biplanar entscheiden (§10.5), das Bild liegt jetzt vor. Ein
  grauerer Fels wäre eine Frage der Wahl des Sets (`rocks_ground_08` ist sandfarben).
- **AO**: übernehmen (dann Snow normieren) oder weglassen (§15.3).
- ~~**WetGround** vom Menschen, danach `stage_polyhaven.py` und `--pack` neu.~~ Entfällt (§16).
- Dauerhafter Speicherort der 2K-Texturen und der Rohquellen (§14.5), BC7 für Arrays (§8.5).

### 15.6 D3D11, D3D12, Vulkan

Auf diesem Gerät nicht lauffähig, also **nicht gerendert**. Rezept auf NN-WS03: §11.4 mit
`cap158auto.ps1`, **vorher** die 18 Dateien von `out/landscape-real/Engine/Textures/Landscape/`
(560 MiB, nicht in git) nach `out/deploy/Editor/EngineContent/Textures/Landscape/` kopieren (nach jedem
Neulinken des Editors neu, §14.5). Die Metal-/GL-Zahlen aus §15.2 sind die Vergleichswerte.
Mips sind hier kein Hindernis: D3D und Vulkan erzeugen für `mipLevels = 1` keine Kette selbst, aber
die drei Arrays, die das Auto-Material liest, tragen 12 gebackene Mips (§14.3).
**Erledigt in Schritt 4 (§17):** auf NN-WS03 nicht übertragen, sondern aus denselben
Poly-Haven-Quellen neu erzeugt (md5-gleich) und gerendert — stimmt mit OpenGL/Metal überein.

## 16. Pfützen als Overlay statt WetGround-Schicht (Thema 177, Schritt 3)

Stand: derselbe Zweig, MacBook Air (Apple M5), Release-Build, **Metal und OpenGL** (kein
D3D11/D3D12/Vulkan, §15.6). Geändert ist der Builder `AutoLandscapeMaterial.cpp`, das
ausgelieferte Asset `M_AutoLandscape.hasset` (neu erzeugt) und ein Test; Arrays, Texturen,
Shader und Backends sind unverändert. Bilder: `docs/img/auto-landscape-pfuetzen-overlay-2026-10-08/`.

### 16.1 Warum

Das WetGround-Set hat der Mensch nicht geliefert (§14.1), und der Platzhalter zeigte am
Pfützenrand ein Schachbrett mit L-Marken (§15.2). Eine eigene Schicht für nassen Boden braucht
außerdem ein weiteres Set mit fünf Maps und drei Texturzugriffe pro Pixel. Nasser Boden ist aber
derselbe Boden, nur dunkler und glatter.

### 16.2 Was jetzt passiert

Die Masken sind unverändert (Rand = `wetMask`, Wasser = `waterMask`, §10.2). Statt `blend(s2, wet,
wetMask)` liegt das Overlay auf **s2**, dem Ergebnis aus Gras/Erde/Fels/Schnee:

| Stufe | Albedo | Rauheit | Normale |
|---|---|---|---|
| Nasser Rand (`wetMask`) | × 0,6 | × 0,5 | die des Bodens |
| Wasser (`waterMask`) | × 0,35 des **trockenen** Bodens | 0,05 | geometrische Normale |

- AO bleibt die des Bodens. Es gibt **keinen neuen Parameter** (weiter 14, `kAutoLandscapeParamCount`):
  0,6 / 0,5 / 0,35 / 0,05 sind feste Werte im Builder.
- Wasser füllt zuerst die tiefen Texel **des Bodens darunter**: Die Höhen-Verschiebung liest
  Masken-B von s2 statt der WetGround-Höhe. Der Wasseranteil ändert sich dadurch leicht (§16.3).
- Der Slice 4 (WetGround) bleibt in den drei Arrays, wird aber **nicht gelesen**.
  `AutoLandscapeLayer::WetGround` bleibt als Slice-Nummer stehen. **Slice 4 aus den Arrays zu
  nehmen ist keine Aufräumarbeit, sondern eine UUID-Kollision:** `landscape_tex_gen` vergibt die
  Array-Ids als `0x400 + Schichten × 3 + Map`. Mit vier Schichten rutschten die Arrays von
  `0x40F..0x411` auf `0x40C..0x40E`, das sind genau die ids der WetGround-Einzeltexturen. Außerdem
  änderte es das Layout der lokalen 2K-Arrays aus Schritt 1 (`out/landscape-real`). Der Preis für
  den toten Slice: 20 % der Array-Größe (≈ 64 MiB von 320 MiB bei 2K).
- **Texturzugriffe: 33 → 30** (gezählt im erzeugten GLSL: 27 `textureGrad` + 3 `texture`;
  ohne Bombing 15 → 12).

### 16.3 Messung

Metal, Draufsicht `HE_DUMP_AUTOLAND` (§10.4, `ana158auto.py masks`), vorher gegen nachher:

| Region Ebene | vorher | nachher |
|---|---|---|
| Erde (R, `=ground`) | 20,5 % | 20,5 % |
| Nasser Rand (G, `=ground`) | 22,9 % | 22,9 % |
| Wasser (B, `=masks`), Anteil > 128 | 12,8 % | 13,1 % |
| Wasser, Mittel B | 29,2 | 30,2 |

Rand, Erde und flacher Boden bleiben **bitgleich**, nur das Wasser ändert sich um 0,3 Punkte (die
Höhen-Verschiebung kommt jetzt vom Gras/Erde-Boden). Alle Erwartungen von `ana158auto.py` sind
erfüllt (Fels, Schnee, Pfützen nur auf Flachem, kein Wasser unter Schnee).

Das Bild (`pfuetzen-vorher-nachher.png`, oben vorher, unten nachher; Metal, deferred, TOD 0,6,
Kamera (−63, 302,6, −28), Nick −20°, Yaw 90°): vorher ein blaues Schachbrett mit gelben L-Marken in
den Pfützen und am Rand, nachher das Gras und die Erde, dunkler und glatter am Rand, dazu eine
Wasserfläche, die den Himmel spiegelt. In der Draufsicht (`pfuetzen-draufsicht-schraeg.png`) werden aus
den dunkelblau gepunkteten Flecken dunkle Bodenflecken in der Farbe des Untergrunds. Grundfarbe und
Rand-Breite wurden nicht nachgestellt, die Faktoren sind eine Geschmacksfrage und ein erster Wert.

| Metal gegen OpenGL (deferred) | Mittel \|d\| |
|---|---|
| Nahaufnahme der Pfützen | 0,007 |
| Schrägansicht Ebene | 0,010 |
| Masken `=masks`, `=ground` | 0,000 |

(Forward-Draufsicht: Ebene 0,000, der Hangfuß unterscheidet sich weiter wie in §15.2 wegen des
GL-Forward-Schattens bei Graph-Materialien.)

### 16.4 Tests

- `Auto landscape material: three arrays, …` prüft jetzt, dass **keine** Array-Lesung den Slice 4
  verlangt (jede Lesung nimmt ihre Slice-Nummer aus einer Konstante darunter; Positivkontrolle:
  Slice 3 wird gelesen; 21 Lesknoten: 3 gebombte Schichten × 3 Maps × (plain + gebombt) + 3 für
  Schnee). **Negativkontrolle:** die `layer(WetGround, …)`-Zeile wieder eingesetzt, der Test wird
  rot (4 Fehler), Zeile wieder raus, grün.
- `The shipped M_AutoLandscape.hasset is exactly what the builder makes` stimmt mit dem neu erzeugten
  Asset überein (Negativkontrolle: mit dem alten Asset schlägt er an).
- Ganze Datei `test_material_graph.cpp` (92 Fälle, 10064 Prüfungen) grün. Das ist **nicht** das ganze
  `he_tests`: Gebaut und gelinkt wurden nur diese Datei und drei Rendering-Hilfsdateien.
  Volle Läufe macht die CI.

### 16.5 Was offen bleibt

- ~~**D3D11/D3D12/Vulkan** nicht gerendert (Schritt 4).~~ Erledigt: §17, bildgleich zu OpenGL
  innerhalb der Toleranz aus §7.5.
- Slice 4 aus den Arrays nehmen: Id-Verschiebung (§16.2), neue Arrays und neue Platzhalter-
  Assets, `HE_DUMP_TEXARRAY`/`HE_DUMP_TEXBOMB` (fünf Streifen) und die `ana158*`-Skripte anpassen.
  Lohnt sich erst zusammen mit der Wahl der Kompression (BC7, §8.5).
- Die Wasserfarbe hängt weiter vom Himmel (§15.2), die Faktoren (0,6 / 0,5 / 0,35) sind ungetunt.

### 16.6 Nebenbefund: das „flackernde” `bmp=NO` von `cap158auto.sh`

Die leere Aufnahme mit 0-Byte-Log (§15.1, Hive-Lesson 132) hat eine feste Ursache: `script(1)` endet
mit Code 1, wenn stdin der von einem Agent-Harness vererbte Deskriptor ist. Mit `</dev/null` am
`script`-Aufruf läuft jede Aufnahme; das Skript tut das jetzt. (Ohne die Umleitung schlugen hier alle
fünf Wiederholungen in Folge fehl, mit ihr gelangen alle 15 Aufnahmen dieses Schritts beim ersten Versuch.)

## 17. Schritt 4: D3D11, D3D12 und Vulkan (Thema 177)

Stand: derselbe Zweig, NN-WS03 (NVIDIA RTX 4070, Windows 11), Release-Build von `HorizonEditor` und
`landscape_tex_gen` in einem eigenen Baum `C:/hw177` (`-DDEPLOY_DIR=C:/hw177/deploy`, damit der
Worktree-Build nicht das Deploy berührt, das der Mensch gerade im Editor laufen hat). Es ist kein
Engine-Code geändert. Rezept: §11.4 mit `cap158auto.ps1`.

### 17.1 Die echten Texturen unabhängig neu erzeugt, nicht übertragen

Die 560 MiB aus `out/landscape-real` (§14.3) liegen nur auf dem Mac, nicht in git und nicht auf
diesem Gerät. Statt sie zu übertragen: dieselben 16 Poly-Haven-Dateien aus §14.1 direkt über die
`api.polyhaven.com/files/<id>`-URLs neu heruntergeladen (4K, dieselben Formate: Diffuse jpg, `nor_gl`
exr, Rough exr bei Grass/Dirt und jpg bei Rock/Snow, Displacement png). **16/16 md5-gleich** zu den
Werten, die `api.polyhaven.com` für diese Dateien meldet — also byteidentisch zu den Quellen, die
Schritt 1 auf dem Mac benutzt hat. `stage_polyhaven.py` (eigene venv, `OpenEXR` hat für Python 3.14
auf Windows ein fertiges Wheel, keine Sonderbehandlung nötig) lieferte dieselben Kennzahlen wie
§14.4: Normal-Renormierung vor der Mittelung 0,874/0,980/0,981/1,018 (Grass/Dirt/Rock/Snow, Doku
„0,87 bis 1,02”), Höhen-Streckung 0,166..0,581 (Grass, Doku „0,17..0,58”) und 0,322..0,918 (Rock,
Doku „0,32..0,92”). `landscape_tex_gen --pack` (Variante **ohne AO**, wie die `real`-Basiswerte aus
§15.2, nicht `realao`: die AO-Frage ist weiter offen, §15.5) schrieb 15/15 Einzeltexturen und 3/3
Arrays, UUIDs `0x40F..0x411`, 2048², 5 Slices, 12 Mips — bitgenau wie §14.3. Gegenprobe mit
`hasset_tex.py`: Mittel der Albedo-Slices Grass (109,9/96,3/61,9), Dirt (99,0/82,2/62,2), Snow
(165,3/165,2/167,2) — alle drei stimmen mit §14.4 überein (Grass „110/96/62”, Dirt „99/82/62”,
Snow „165”). Die 18 `.hasset`-Dateien ersetzten die Platzhalter in `C:/hw177/deploy/Editor/
EngineContent/Textures/Landscape/` (die Arrays dort danach 107 MiB statt 437 KB, geprüft). Der
Build enthält den Zeugen (`findstr “AUTOLAND witness” HorizonEditor.exe` trifft).

### 17.2 Aufnahmen

`cap158auto.ps1 -Backends OpenGL,D3D11,D3D12,Vulkan`, Modi `masks ground normal surface` (je einmal)
und `1 nobomb plaingraph builtin` (je einmal ohne Zusatz und einmal mit
`-Extra @{HE_DUMP_SHADOW='0.1'} -Tag _s01`, siehe §11.3/§11.4) — 48 Läufe, dazu ein D3D12-Lauf mit
`HE_GPU_DEBUG=1` (Debug-Layer + DRED). **Alle 49 Läufe: `bmp=True`, kein `[ERROR]`**, der Zeuge
meldet durchgehend `3 graph textures, array mask 7` bei Modus `1`/`nobomb` (alle drei Arrays live,
heTexP0..2) und `14`/`13` Parameter wie in Schritt 3. Vulkan-Validation: in allen 13 Vulkan-Logs
genau die eine bekannte Info-Zeile `validation layer ENABLED`, sonst nichts (§8.3/§12-Präzedenz).

### 17.3 Masken-Orakel

`ana158auto.py masks` auf `masks`/`ground`, alle vier Backends: **alle 15 Erwartungen erfüllt**,
auch der Pfützenrand (`wet` auf der Ebene: 22,9 % auf allen vier Backends — exakt der Wert aus
Schritt 3, §16.3, Metal/GL). D3D11 und D3D12 sind untereinander **md5-gleich**, Vulkan weicht nur in
Rundungsfehlern ab (siehe §17.5).

### 17.4 Bildvergleich gegen OpenGL — zwei verschiedene Geschichten

**Unlit-Debugansichten** (`masks`, `ground`, `normal`, `surface`, keine Beleuchtung im Spiel):
mean|Δ| 2,8 / 3,2 / 0,001 / 1,33, **0,000 % Pixel > 8** auf allen drei Backends. Das ist derselbe
feste Schwarzpegel-Versatz (0 → 4 von 255), den §12 für die alten Platzhalter-Texturen schon
dokumentiert hat, kein neuer Befund und ohne jede Wirkung auf den Pixel-Anteil-Test.

**Modus `1`/`nobomb` mit Standard-Schattendistanz:** mean|Δ| 2,5 / 3,4 insgesamt, aber **konzentriert
in der Zone `foot`** (74,9 / 78,2 mean|Δ|, 100 % > 8) — das ist §11.3: OpenGL zeichnet im
Forward-Pfad für Graph-Materialien **keinen** Sonnenschatten, D3D11/D3D12/Vulkan tun es. Kein
Materialfehler, sondern der bekannte, hier erneut bestätigte Pfadunterschied. Die anderen drei
Zonen liegen bei 0,00–0,04.

**Modus `1`/`nobomb`, schattenneutralisiert (`_s01`, die eigentliche Materialprobe):**

| Vergleich (D3D11/D3D12/Vulkan gegen OpenGL) | mean\|Δ\| | Pixel > 8 |
|---|---|---|
| `1_s01` | 0,005–0,006 | 0,000 % |
| `nobomb_s01` | 0,004 | 0,000 % |
| `plaingraph_s01` (Kontrolle) | 0,001 | 0,000 % |
| `builtin_s01` (Kontrolle, Standardmaterial) | 45,8 | 99,9 % |

`builtin` weicht erwartungsgemäß stark ab (kein Teil dieses Themas, dieselbe Kontrolle wie in §12/§13:
das eingebaute Terrain-Material behandelt Umgebungslicht auf D3D/Vulkan anders). Für das
Auto-Material selbst liegt die Abweichung bei 0,004–0,006 — eine Größenordnung unter der
Toleranz aus §7.5 (mean|Δ| ≤ 1,0, ≤ 0,5 % > 8) und im selben Rauschband wie die
D3D12-Debug-Layer-Gegenprobe (§17.5). **Die echten Texturen, die drei Arrays und das
Pfützen-Overlay aus Schritt 3 rendern auf D3D11, D3D12 und Vulkan praktisch bitgleich zu OpenGL.**

### 17.5 Schatten- und Bombing-Orakel, Gegenproben

- **Schatten vorhanden:** `ana158auto.py shadow` bestätigt für Modus `1`/`nobomb`: OpenGL-forward
  Verhältnis 1,00 (`MISSING`, wie erwartet), D3D11/D3D12/Vulkan 0,37 (Modus `1`) bzw. 0,32
  (`nobomb`), alle `PRESENT`.
  **Nachtrag gegenüber §12:** Das Verhältnis 0,37 trifft jetzt genau Metals alten Wert aus §11.2
  (0,37 forward/deferred), nicht mehr die alten 0,30 aus §12, die vor dem Merge von PR #98
  (Graph-Material SkyEnv/AO auf Vulkan/D3D11/D3D12) gemessen wurden. PR #98 ist auf diesem Zweig
  bereits gemergt (siehe Commit-Liste oben); die alte Abweichung „kein Himmels-IBL für
  Graph-Materialien” aus §12/§13 gilt für diesen Zweig **nicht mehr**.
- **Bombing-Orakel** (`ana158auto.py repeat`, `1_s01` gegen `nobomb_s01`): mit Bombing keine
  periodischen Minima (Oszillation 0,03/0,01), ohne Bombing Minima bei 12/25/37 px (Oszillation
  0,19/0,10) — auf **allen vier Backends identisch**, bis auf die dritte Nachkommastelle.
- **D3D12 mit `HE_GPU_DEBUG=1`** (Debug-Layer + DRED), Modus `1`: 0 zusätzliche Meldungen, Bild
  **md5-gleich** zum Lauf ohne Debug-Layer.
- **D3D11 = D3D12** (md5-gleich) bei `1`, `nobomb` und deren `_s01`-Varianten. Vulkan weicht in
  Rundungsfehlern ab (eigene md5, mean|Δ| gegen D3D11/D3D12 < 0,01 in allen vier Debug-Modi),
  derselbe Compiler-Unterschied wie in §7.5/§13.

### 17.6 Was das belegt, was nicht

Belegt: Die echten Poly-Haven-Texturen, die drei Textur-Arrays (`heTexP0..2`, Array-Maske 7, 107 MiB
je Array mit 12 gebackenen Mips) und das Pfützen-Overlay aus Schritt 3 laden und rendern auf D3D11,
D3D12 und Vulkan **ohne stillen Rückfall auf eingebautes PBR** (Array-Maske und Parameterzahl im
Zeugen stimmen durchgehend), ohne Vulkan-Validation-Meldung und ohne D3D12-Debug-Layer-Meldung, und
stimmen mit OpenGL im Rahmen der Toleranz aus §7.5 überein, sobald der bekannte GL-Forward-Schatten-
Unterschied (§11.3) herausgerechnet ist. Bombing und Pfützenränder messen auf allen vier Backends
dieselben Zahlen wie auf Metal/GL in Schritt 2/3 (22,9 % wet, Masken-Erwartungen). CI auf diesem
Zweig (Lauf 37822782616): **alle vier Jobs grün** (Linux, Linux · Vulkan lavapipe, macOS, Windows).

**Nicht** geprüft: die drei offenen Geschmacksentscheidungen aus §15.5 (Schnee bomben, Fels
Triplanar/Biplanar, AO übernehmen) — die bleiben für Schritt 4 unverändert offen. Kein Metal auf
diesem Gerät (siehe §11.2/§13.4 für den Metal-Nachweis mit der alten Platzhalter-Textur; ein
Metal-Lauf mit den echten Texturen ist bereits in Schritt 2/3 erfolgt). Kein Mip-Flimmern geprüft
(der Draufsicht-Zeuge löst Minifikation in der Ferne nicht auf; die Arrays backen ihre eigene
Mip-Kette, unabhängig vom Backend, §8.1/§15.6). Leistung/GPU-Speicher mit den 2K-Arrays weiter nicht
gemessen (§15.4).

