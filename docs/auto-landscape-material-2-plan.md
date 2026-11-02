# Auto-Landschaftsmaterial 2.0: Bestand und Entwurf

Thema 180, Schritt 1 · Zweig `claude/auto-landscape-material-2-0-alle-backends-bcn-instancing-audit` @ `7b8a1b9b`
· Vergleichsstand `origin/release/0.7.0` @ `4d8772d2` · 2026-10-20 · **in diesem Schritt ist kein Code geändert**.

Was das Auto-Landschaftsmaterial (`docs/auto-landscape-material-textures.md`) bekommt: mehrere Texturvarianten je Schicht,
prozedurale Meshes je Schicht über den vorhandenen Foliage-Pfad, BCn-Kompression für die vergrößerten Arrays, und alles
auf Metal, OpenGL, Direct3D 11, Direct3D 12 und Vulkan. Dieses Dokument ist das Soll. Die Messwerte des Performance-Audits
gehören nicht hierher (Schritt 6), die Umsetzung in die Schritte 2 bis 5.

Es ersetzt zwei Aussagen der Textur-Doku: „keine Varianten (kein ‚Stein 2‘)“ (dort §2.3, §4.3) und „BC7/ASTC für Arrays
fehlt noch“ (dort §8.1, §8.5). Nicht Teil des Themas: Wetter- und Wasserfunktionen. Die Pfützen des Materials bleiben, wie sie sind.

---

## 0. Kurz

| # | Entscheidung | Abschnitt |
|---|---|---|
| D1 | **Varianten sind Slices, keine Sampler.** Die drei Arrays bleiben `heTexP0..2`. Kein neuer Sampler, kein Register verschiebt sich. D3D11/FXC SM5.0 bleibt bei 16 von 16 und ist damit gelöst, weil nichts dazukommt | 3 |
| D2 | **`heTexP3` bleibt frei.** Es ist das einzige freie Material-Register (D3D s7, Metal 4). Es wird auch von Version 2.0 nicht gebraucht | 3.4 |
| D3 | **Layout:** Tiefe des Arrays = 4 · V + 1 (Gras, Erde, Fels, Schnee je V Varianten, dahinter der ungelesene Platzhalter für nassen Boden). V = `Tiefe / 4` wird im Shader aus `textureSize` gelesen. V = 1 ist exakt das heutige Layout. **Soll V = 3** (13 Slices), Obergrenze V = 4 | 5 |
| D4 | **Variantenwahl pro Hex-Tap**, nicht pro Pixel, aus den bisher ungenutzten unteren 8 Bit von `heBombHash`. Kein zusätzlicher Texturzugriff, keine sichtbare Naht | 6 |
| D5 | **BCn je Karte:** Albedo BC7, Normal **BC5** (nur XY, Z wird im Shader rekonstruiert), Maske **BC7** (drei gepackte Kanäle, BC4 reicht dafür nicht). BC1/BC3 als Rückfall, ASTC 4×4 auf Apple. **BC4 und BC5 sind neue Arbeit** (Enum, Wrapper, Upload auf fünf Backends), die Block-Encoder selbst liegen schon im Repo | 7 |
| D6 | **Komprimiert wird im Asset**, nicht nur beim Export. Sonst bleibt der Editor auf Windows/Linux bei 832 MiB RGBA8 (V = 3, 2K). Empfehlung: Geschwister-Assets je Format-Familie, vom Renderer nach Fähigkeit gewählt | 7.5 |
| D7 | **Layer-Meshes sind `FoliageLayer`s.** Kein zweiter Streu- oder Zeichenpfad. Regeln (Hang, Höhe, Oberfläche) kommen als Felder an `FoliageLayer`. Die Meshes (Felsbrocken, Büschel, Kiesel) schreibt `mesh_gen` deterministisch mit eigenen LOD-Stufen | 8 |
| D8 | **Dieser Zweig muss vor Schritt 4, besser vor Schritt 2, `origin/release/0.7.0` einmergen.** Foliage-Schichten, Cluster, LOD-Stufen und Graph-Instancing liegen nur dort | 1 |
| D9 | **Das Parameter-Limit (`kMatMaxParams = 16`) wird nicht erreicht:** 13 belegt, Variation braucht 0 neue, die optionale Tönung höchstens 2. Kein Zusammenstoß mit Thema 229 | 6.4 |

---

## 1. Stand der Zweigbasis und Zitierweise

Der Zweig steht **719 Commits hinter** `origin/release/0.7.0` (`git rev-list --count HEAD..origin/release/0.7.0`, 3 Commits
davor). Ein Probe-Merge (`git merge-tree --write-tree HEAD origin/release/0.7.0`) hat **einen** Konflikt in
`tests/test_material_graph.cpp`; `docs/auto-landscape-material-textures.md`, `src/HE_Editor/EditorApplication.cpp` mischen sich
selbst. Der Merge ist **nicht** gemacht (der Schritt verbietet es, und er ist Sache der Queen).

Was das für den Entwurf heißt:

- Material, Arrays, Texturkompression und Sampler-Pins sind auf beiden Ständen gleich (die Pin-Tabelle ist unverändert,
  `git diff HEAD origin/release/0.7.0 -- MaterialShaderLibrary.cpp` ändert nur die instanzierten Vertex-Zwillinge und einen
  Uniform-Block, keine Sampler).
- **Foliage ist auf diesem Zweig noch die alte Einzelschicht** (`src/HE_Scene/include/HorizonScene/Components/FoliageComponent.h`:
  eine Schicht, `cachedInstances`, ein `RenderObject` je Pflanze in `RenderExtractor.cpp:621`). Schichten, Cluster, LOD-Stufen,
  Graph-Material-Instancing und die Instanz-Arena gibt es nur auf `origin/release/0.7.0`.

**Zitierweise:** `pfad:zeile` meint den Zweig-Stand `7b8a1b9b`. `R:pfad:zeile` meint `origin/release/0.7.0` @ `4d8772d2`.
Jede genannte Stelle ist gegen den jeweiligen Stand geprüft; die `R:`-Stellen lassen sich im Arbeitsbaum erst nach dem Merge
greppen (bis dahin `git show origin/release/0.7.0:<pfad>`).

---

## 2. Bestand

### 2.1 Das Auto-Material

- Quelle `HE::buildAutoLandscapeGraph` (`src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp`, 414 Zeilen), ein gewöhnlicher
  Graph aus Standardknoten; kein Backend-Code. Asset `Engine/Materials/M_AutoLandscape.hasset`.
- Texturen: drei Arrays (`kAutoLandscapeAlbedoArray/NormalArray/MaskArray`, `AutoLandscapeMaterial.h:83-85`), Slice-Reihenfolge
  `AutoLandscapeLayer { Grass 0, Dirt 1, Rock 2, Snow 3, WetGround 4 }` (`AutoLandscapeMaterial.h:97`). Der Slice ist heute eine
  **Konstante je Schicht** (`AutoLandscapeMaterial.cpp:110-112`, Lambda `layer`), gelesen über *Texture Array Bombing* /
  *Normal Map Array Bombing* mit einem Hex-Gitter je Schicht (Seeds 11/23/37/53, `:138-143`), umschaltbar über den Static Switch
  „Texture Bombing“ (`:127`).
- Slice 4 (WetGround) wird nicht gelesen (`:144`). Er bleibt im Array, damit Layout und feste UUIDs `0x40F..0x411` nicht wandern.
- Der Dreck-Fleck-Fbm nutzt den Integer-Hash (`:164-166`, `p[0] = 1.0f`; Emitter `MaterialGraph.cpp:1727` `heHashI2`).
- Parameter: 13 (`kAutoLandscapeParamCount`, `AutoLandscapeMaterial.h:115`) von `kMatMaxParams = 16`
  (`MaterialGraph.h:533`): **3 freie Slots**. Der Static Switch kostet keinen.
- 12 gebombte Lesungen (4 Schichten × 3 Karten) × 3 Hex-Taps = 36 Texturzugriffe je Pixel im Gebombt-Modus.
  Die Textur-Doku nennt 30 (Schnee war damals plain); die Zahl ist durch Schnee-Bombing überholt (`AutoLandscapeMaterial.cpp:141-143`).
  Genau nachzählen ist Sache von Schritt 3.
- Das Material liest `heLandscapeWeights` (malbare Schichten, `kAutoLandscapePaintLayerNames`, `AutoLandscapeMaterial.h:90`).
  Auf D3D ist s0 damit **live** belegt.

### 2.2 Arrays im Asset und in den fünf Backends

- `TextureAsset::layers` (`Assets.h:617`, Doku-Kommentar `:604-616`): Slices hintereinander, **jede mit eigener Mip-Kette**
  (D3D-Subresource-Reihenfolge). Bauen: `HE::buildTextureArray` (`src/HE_Core/src/ContentManager/TextureArrayBuild.cpp:59`),
  Prüfung `textureArrayPayloadValid` (`:26`) verlangt `channels == 4` und `format == RGBA8`. Alle Slices gleich groß, gleiches Format.
- Codegen: `textureSampler(...)` (`MaterialGraph.cpp:610`) vergibt Slots `heTexP0..3` (`kMatMaxGraphTextures = 4`,
  `MaterialGraph.h:506`), ein Array belegt denselben Slot wie ein 2D, nur als `sampler2DArray`. Rundung/Klemme des Slice:
  `arrayCoord` (`MaterialGraph.cpp:633`) und im Bombing-Emitter `emitBombNode` (`:906`): `clamp(floor(s + 0.5), 0, textureSize(..).z - 1)`.
- Die Backends laden Arrays **nur RGBA8** und lehnen Blockformate ausdrücklich ab:

| Backend | Array-Upload | Stelle der RGBA8-Beschränkung |
|---|---|---|
| OpenGL | `uploadTextureArrayGL` (`OpenGLRenderer.cpp:8264`), `GL_TEXTURE_2D_ARRAY` | `:8270` (`textureArrayPayloadValid`), Blockformate nicht array-fähig (Kommentar `:8262`) |
| D3D11 | `ArraySize = n`, SRV `TEXTURE2DARRAY` (`D3D11Renderer.cpp:4400-4412`) | `:4436` „arrays are RGBA8 only“ |
| D3D12 | `DepthOrArraySize = n` (`D3D12Renderer.cpp:3907-3919`) | `:3919` |
| Vulkan | `arrayLayers = n` (`VulkanRenderer.cpp:7026-7032`) | `:7032` (`isBlock \|\| !payloadValid`) |
| Metal | `MTLTextureType2DArray` (`MetalRenderer.mm:9280-9290`) | `:9290` (`textureArrayPayloadValid`) |

### 2.3 Texturkompression heute

- Formate: `enum class TextureFormat : uint8_t { RGBA8 = 0, ASTC_4x4 = 1, BC7 = 2, BC3 = 3 }` (`Assets.h:581`); die Byte-Mathematik nimmt
  überall **16 Byte je 4×4-Block** an (`textureFormatIsBlock4x4`, `Assets.h:584`).
- Encoder (`src/HE_Core/src/Hpak/HpakWriter.cpp`): `encodeAstc4x4`, `encodeBc7` (`:101`, `vendor/bc7enc`), `encodeBc3` (`:137`, `stb_dxt`).
  Gewählt in `cookTexture` (`:703`), Schalter `encodeLevel` (`:765-776`, Werte 1..3).
- **BC4 und BC5: kein Wrapper, kein Enum-Wert, kein Upload.** Der Repo-Text „kein BC4/BC5-Encoder“ stimmt für die Anbindung, nicht für die
  Bibliothek: die vendorte `src/HE_Core/vendor/bc7enc/stb_dxt.h` ist **v1.12** und enthält `stb_compress_bc4_block` / `stb_compress_bc5_block`
  (Deklaration `:59-60`, Implementierung `:617`/`:622`). BC1 steckt als `stb_compress_dxt_block(..., alpha = 0, ...)` darin.
- **Arrays werden nie gekocht**: `cookTexture` gibt für `layers > 1` `{}` zurück (`HpakWriter.cpp:730-735`), der Packer liefert sie unverändert aus.
- Laufzeit je Backend (Formatwahl): GL `glSupportsBptc` (`OpenGLRenderer.cpp:8153`), `glSupportsS3tc` (`:8166`), `uploadTextureAssetGL` (`:8187`);
  D3D11 `D3D11Renderer.cpp:4419-4420`; D3D12 `D3D12Renderer.cpp:3926-3927`; Vulkan `VulkanRenderer.cpp:7022-7023` mit
  `vkGetPhysicalDeviceFormatProperties`-Wächter; Metal `metalTexPixelFormat` (`MetalRenderer.mm:9186-9210`, ASTC über `supportsFamily:Apple2`,
  BC über `supportsBCTextureCompression`). Der Export wählt: Apple+Metal → ASTC, Apple+GL → BC3, Windows/Linux → BC7
  (`docs/asset-optimization-plan.md`, Abschnitt „GPU-Textur-Kompression“). Ein Pak trägt **ein** Format je Textur.

### 2.4 Instancing und Foliage (nur auf `origin/release/0.7.0`)

Die Doku `docs/gpu-instancing-cross-backend-plan.md` (06.09.) ist in einem Punkt überholt: Foliage ist seitdem durch Thema 182 neu gebaut
(`R:docs/foliage-instancing-lod-plan-2026-10-10.md`, §14 bis §21). Der Stand für dieses Thema:

- **Daten:** `FoliageComponent::layers` (≤ `kMaxLayers = 32`, `R:FoliageComponent.h:104`), jede `FoliageLayer` (`R:FoliageComponent.h:35`) mit Mesh, Material, Dichte,
  Seed, Skala, `drawDistance`, Maske, `castsShadow`, `contributesAO`, `shadowDistance`, `lods` (`FoliageLod`, `:18`, ≤ `kMaxLods = 7`) und `lodHysteresis`.
  Der Foliage-Plan reserviert ausdrücklich: *Regeln für Hang, Höhe und Paint-Schicht sind für Thema 180 reserviert* und
  *Thema 180 baut keinen zweiten Pfad* (`R:docs/foliage-instancing-lod-plan-2026-10-10.md` §5, §6.2).
- **Streuen:** `FoliageSystem::update` (`R:FoliageSystem.cpp:238`) → `scatterLayer` (`:140`), terrain-lokal in `FoliageStore` (Buckets zu 32 m, `R:FoliageStore.h:21-27`).
- **Extraktion:** `extractFoliage` (`R:FoliageExtract.cpp:399`): je sichtbarem Bucket **ein Cluster-`RenderObject`** (`emit`, `:526`; `instanceBlock`, `:538-540`).
- **Aufklappen** in genau zwei Funktionen: `GeometryPass::execute` (`R:RenderPass.cpp:16`, Lauf `:122-153`, Cap-Schnitt bei
  `HE::kMaxInstancesPerDraw = 65536`, `R:RenderConstants.h:31`) und `RenderSorter::batchDepthRuns` (`R:RenderSorter.cpp:78`, `:100`), beide über
  `RenderWorld::appendInstances` (`R:RenderWorld.h:344`). Backends sehen weiter `DrawCall::instanceTransforms`.
- **Instanz-Speicher je Backend** (alle 128 Byte je Instanz `{mvp, model}`, außer wo vermerkt):

| Backend | Puffer | Stelle |
|---|---|---|
| OpenGL | `m_instanceVBO` (Attribute 4 bis 7, Divisor 1), `GL_STREAM_DRAW` | `R:OpenGLRenderer.cpp:3410`, Draw `glDrawElementsInstanced` `:3974` |
| D3D11 | `instanceSB` (dynamischer StructuredBuffer) und `ensureMatInstVB` für die Graph-Zwillinge | `R:D3D11Renderer.cpp:1340`, `:1409` |
| D3D12 | Instanz-Arena (`allocInstances`) | `R:D3D12Renderer.cpp:3582` |
| Vulkan | Instanz-Arena (`allocInstances`) | `R:VulkanRenderer.cpp:7002` |
| Metal | transienter Puffer an Vertex-Buffer 5 (`UploadTransientBytes`) | `R:MetalRenderer.mm:8881` |

- **Graph-Material-Instancing** (Wind, Cutout): `customVertexInstanced` / `standardVertexInstanced` (`R:MaterialShaderLibrary.cpp:987`, `:1102`), auf allen
  fünf Backends gebaut. **Ausnahme:** gebackene Pak-Varianten haben keinen instanzierten Vertex, ein exportiertes Spiel im Forward-Pfad (Standard) schleift
  Graph-Foliage weiter je Pflanze (`R:docs/foliage-instancing-lod-plan-2026-10-10.md` §15.2, Anfrage 105 der Queen offen).
- **Kein Impostor-Baking** gebaut (`R:…plan…` §16.3): LOD heißt Mesh-Stufen, Dichte-Ausdünnung und die statische Kreuz-Karte `kEngineCrossCardMeshId = 0x108`
  (`R:DefaultAssets.h:104`).
- **Geprüft und nicht wiederverwendbar:** `R:src/HE_Rendering/src/GiInstanceSurface.cpp` (`giInstanceSurface`, `:12`) ist die Faltung der Oberflächenfarbe für die GI-Näherung,
  kein Instanz-Puffer. Cluster gehören ohnehin nicht in die GI-Instanzlisten (`R:…plan…` §6.3, Punkt 2).

---

## 3. Sampler-Budget

### 3.1 D3D (SM 5.0): die Tabelle aus `kHlslMaterialPins`

`MaterialShaderLibrary.cpp:2711` (`kHlslMaterialPins`), Begründung im Kommentar `:2650-2710`. Der GLSL-Binding-Wert wird 1:1 zum t-Register,
nur die **Sampler-Hälfte** zieht um:

| Sampler | Binding (GLSL) | Inhalt | Art |
|---|---|---|---|
| s0 | 14 **und** 16 | `heLandscapeWeights` (**live**) und `heAO` (nur deklariert, per `texelFetch` gelesen, **tot**) | Engine |
| s1 | 17 | DDGI-Irradianz | Engine |
| s2 | 2 | `heTex0` (Legacy-/Mesh-Textur) | Material |
| s3 | 18 | DDGI-Sichtbarkeit | Engine |
| s4 / s5 / s6 | 4 / 5 / 6 | `heTexP0` / `heTexP1` / `heTexP2` (Albedo-, Normal-, Masken-Array) | **Material** |
| s7 | 7 | `heTexP3` | **Material, frei** |
| s8 | 31 | Forward-SSR | Engine |
| s9 | 32 | GI-Reflexion | Engine |
| s10 / s11 | 10 / 11 | GI-Schattenmasken Sonne / lokal | Engine |
| s12 | 12 | CSM-Array | Engine |
| s13 | 13 | Schattenatlas Punkt/Spot | Engine |
| s14 | 33 | Wolkenschatten | Engine |
| s15 | 15 | Sky-Env-Cubemap | Engine |

Nachgezählt gegen die Pin-Liste: zehn Identitäts-Pins (Bindings 2, 4, 5, 6, 7, 10, 11, 12, 13, 15) plus sieben umgezogene (14, 16, 17, 18, 31, 32, 33) sind
**17 kombinierte Sampler auf 16 Registern**. Das passt nur, weil `heAO` keinen *lebenden* Sampler braucht (FXC lehnt zwei lebende Nutzer auf einem Register ab,
X4500, aber nicht eine tote Deklaration neben einer lebenden; der Registertest in `tests/test_material_graph.cpp` prüft genau diese Regel, nicht eine Ausnahmeliste).
**s0..s15 sind voll.** Ein 17. lebender Sampler lässt jedes Graph-Material auf D3D11 und D3D12 still auf das eingebaute PBR zurückfallen.

### 3.2 Metal, OpenGL, Vulkan

- **Metal** (`MaterialShaderLibrary.cpp:2553-2625`): 16 Sampler 0 bis 15, ebenfalls voll. `heTexP0..3` liegen auf Sampler/Textur **1 bis 4**, `heTex0` auf 0,
  Weightmap 13, Sky 14, AO 15, DDGI 6/7, Reflexion 9/10, GI-Masken 5/8, CSM 11, Lokalschatten 12. Die Wolkenschatten-Textur hängt an Textur 16 mit
  Inline-Sampler (kein Sampler-Slot).
- **OpenGL:** auf dem M5 (macOS GL 4.1 über Metal) **gemessen** `GL_MAX_TEXTURE_IMAGE_UNITS = 16` je Fragment-Stufe (Wegwerfprogramm unter `/tmp`, nicht im Repo).
  Das Auto-Material rendert dort bereits (Textur-Doku §10.4), die Zahl ändert nichts, solange 2.0 keinen Sampler hinzufügt. Treiber auf Windows/Linux: *nicht geprüft*.
- **Vulkan:** der Materialsatz braucht 17 Fragment-Sampler (`HE::vkmat::fragmentSamplerCount(true)`, `src/HE_Rendering/include/Backends/Vulkan/VulkanMaterialLayout.h:127`).
  Das ist eins über dem Spec-Minimum; ein Gerät genau am Minimum bekommt das Layout ohne die Weightmap-Zeile (`landscapeWeightsFit`, `:140`, Log `VulkanRenderer.cpp:2645-2660`).
  Varianten ändern an diesem Layout nichts.

### 3.3 Die Lösung: mehr Slices, nicht mehr Sampler

Ein Array belegt **einen** Sampler, egal wie viele Slices. Mehr Varianten je Schicht heißen deshalb nur mehr Tiefe in den **drei vorhandenen Arrays**.
Konsequenzen:

- s0, s1 bis s3, s8 bis s15 und die Metal-Slots bleiben, wie sie sind. Die Pin-Tabellen (`:2572-2625`, `:2711-2731`), das Vulkan-Layout und die
  D3D12-Root-Signature ändern sich nicht. Der Registertest bleibt der Wächter und muss grün bleiben.
- Das Material liest weiter dieselben 36 (gebombt) Texturzugriffe: die Variante wählt nur den Slice *innerhalb* eines Taps (§6).
- Was **nicht** geht und deshalb nicht geplant wird: eine fünfte Projekttextur (`kMatMaxGraphTextures = 4`, `MaterialGraph.h:506`, und das D3D-Budget wäre
  gesprengt) oder eine zweite Weightmap als eigene Textur (Textur-Doku §7.2, derselbe Grund).

### 3.4 Was mit `heTexP3` passiert

`heTexP3` (D3D s7, Metal 4, GLSL-Binding 7) **bleibt frei**. Sein Sampler ist in der Tabelle schon bezahlt; ein Array dort würde s0 und die Engine-Slots nicht berühren.
Der einzige legitime Verbraucher wäre eine Aufteilung der Maske in BC5 (AO, Rauheit) plus BC4 (Höhe), also ein viertes Array und fünf statt vier Lesungen je Tap.
Das wird **nicht** gebaut (§7.2). Der Slot bleibt für spätere, nicht zu diesem Thema gehörende Zwecke (z. B. ein Wellen-Normal, Textur-Doku §2.2) reserviert.

---

## 4. Array-Grenzen

Gebraucht werden bei V = 4 genau 17 Slices (bei V = 8 noch 33). Das liegt unter dem **garantierten Minimum jeder API**; keine Variantenzahl in diesem Entwurf
kommt in die Nähe eines Limits.

| Backend | Max. Array-Ebenen | Quelle / Status |
|---|---|---|
| OpenGL | `GL_MAX_ARRAY_TEXTURE_LAYERS`, Spec-Minimum 256 | **gemessen 2048** auf macOS GL 4.1 / Apple M5. Windows/Linux-Treiber: *zu prüfen* |
| Direct3D 11 | `D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION` = 2048 (Feature Level 11) | aus der API-Spezifikation, **nicht gegen einen Header geprüft** (kein Treffer im Quellbaum) |
| Direct3D 12 | `D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION` = 2048 | wie D3D11, *zu prüfen* |
| Vulkan | `maxImageArrayLayers`, Spec-Minimum 256, Desktop-Treiber meist 2048 | der Code liest nur die Sampler-Grenzen (`VulkanRenderer.cpp:2645`), nicht die Ebenenzahl. lavapipe: *zu prüfen* |
| Metal | Array-Länge bis 2048 | aus den Metal-Feature-Tabellen, *zu prüfen* (nicht gemessen) |

Weitere Regeln, die der Entwurf einhält:

- **Alle Slices eines Arrays haben dieselbe Breite, Höhe, Mip-Zahl und dasselbe Format** (erzwingt `buildTextureArray`, `TextureArrayBuild.cpp:59`). Alle Varianten einer Karte also 2048².
  Das gilt auch für die drei Arrays untereinander: gleiche Tiefe (§5, Wächter).
- Slice-Reihenfolge slice-major, **jede Slice mit eigener vollständiger Mip-Kette** (D3D-Subresource-Reihenfolge). Mit Blockformaten ist die Byte-Größe je Ebene
  `ceil(w/4) · ceil(h/4) · Blockbytes` (BC1/BC4: 8, sonst 16). `textureArraySliceBytes` (`TextureArrayBuild.cpp:8`) muss dafür formatbewusst werden.
- Die kleinsten Mips (2×2, 1×1) liegen unter der Blockgröße. D3D und GL runden auf einen Block auf; ob alle fünf Backends das mit Array-Ressourcen gleich tun, ist *zu prüfen* (Schritt 2, Negativkontrolle mit 128²-Arrays).
- Texturgröße: 2048² ist weit unter jedem Limit (GL gemessen 16384).

---

## 5. Texturvarianten: Zahl, Layout, Speicher

### 5.1 Layout

```
Tiefe = 4 · V + 1
Slice(Schicht g, Variante v) = g · V + v          g = 0 Gras, 1 Erde, 2 Fels, 3 Schnee
Slice 4 · V = WetGround (wird nicht gelesen, bleibt für Layout und UUIDs)
V = textureSize(Array, 0).z / 4          (ganzzahlige Division im Shader)
```

V = 1 ergibt die heutigen fünf Slices, Slice für Slice (`Slice = g`). Weil V aus der Tiefe kommt und nicht im Graph eingebacken ist, rendert **dasselbe Material**
mit den eingecheckten 5-Slice-Platzhalter-Arrays (128², `T_Landscape_*_Array`, V = 1, wie heute) und mit den echten 13-Slice-Arrays (V = 3). Ein veraltetes oder kürzeres
Array kann so keinen falschen Slice klemmen (die „weißes Array = Sechsecke“-Fehlerklasse aus den Lektionen). Die drei Arrays brauchen **gleiche Tiefe**:
`landscape_tex_gen` schreibt sie nur gemeinsam, ein Test prüft die Gleichheit, der Renderer meldet eine Abweichung einmal im Log.

### 5.2 Speicher (2K, Mips vollständig)

Eine Slice 2048² RGBA8 mit Mips = **21,33 MiB** (Textur-Doku §5). BC7, BC5 und ASTC 4×4 kosten 1 Byte je Texel, also **5,33 MiB**; BC1 und BC4 0,5 Byte, **2,67 MiB**.

| V | Slices | RGBA8 je Array | RGBA8, drei Arrays | BC7 / BC5 / BC7 je Array | BC, drei Arrays |
|---|---|---|---|---|---|
| 1 (heute) | 5 | 106,7 MiB | **320 MiB** | 26,7 MiB | **80 MiB** |
| 2 | 9 | 192,0 MiB | 576 MiB | 48,0 MiB | 144 MiB |
| **3 (Soll)** | 13 | 277,3 MiB | **832 MiB** | 69,3 MiB | **208 MiB** |
| 4 (Obergrenze) | 17 | 362,7 MiB | 1088 MiB | 90,7 MiB | 272 MiB |

- 1K ist ein Viertel, 4K das Vierfache (V = 3 in 4K komprimiert: 832 MiB, nicht zu empfehlen).
- Albedo als BC1 (kein Alpha nötig) spart bei V = 3 noch einmal 34,7 MiB, kostet aber Qualität: nur als Qualitätsstufe „niedrig“.
- Der Unterschied 832 gegen 208 MiB bei V = 3 ist der Grund für D6: ohne komprimierte Assets bleibt der Editor auf einer 6-GB-Karte nicht bezahlbar.
- Quellmaterial für V = 3: 12 Sets × 3 Karten = 36 Dateien plus die 3 Platzhalter für nassen Boden (Beschaffung: eigener Schritt, §12).

---

## 6. Variationsmethode

### 6.1 Pro Hex-Tap, nicht pro Pixel

Das Bombing mischt je Pixel **drei** Hex-Zellen (`heBombGrid`, `MaterialGraph.cpp:1811`). Würde man **einen** Slice je Pixel aus Rauschen oder Weltposition wählen, entstünde eine harte
Texturnaht überall dort, wo der Slice mitten in einer Mischung wechselt. Deshalb wählt **jeder der drei Taps seine Variante aus dem Hash seiner eigenen Zelle**: die Naht ist dann
die bestehende, weiche Hex-Mischung, und es kommt **kein Texturzugriff** dazu (36 bleiben 36). Die Weltposition geht über den Zellindex ein (die Zelle ist ein Stück Weltraum),
das Rauschen ist `heBombHash`.

### 6.2 Der Hash

`heBombHash(ivec2 cell, uint seed)` (`MaterialGraph.cpp:1762`) liefert `uvec3 h`. `heBombTap` (`:1803`) nimmt `h.xy >> 8` für den Versatz und `h.z >> 8` für die Drehung.
Die **unteren 8 Bit** sind frei:

```
uint vsel = ((h.x & 255u) * uint(V)) >> 8u;      // 0 .. V-1, reine Ganzzahlrechnung
slice_tap = clamp(group * V + vsel, 0, depth - 1)
```

Ganzzahlig, damit D3D, Vulkan, GL und Metal bit-genau dieselbe Variante ziehen (das Lehrstück des Integer-Hashs aus Thema 158, Schritt 7/8, gilt hier ebenso).
Der Hash ist eine pcg-artige Mischung mit `v ^= v >> 16`, die unteren Bits sind also durchmischt; das beweist ein Test (Histogramm über 10⁵ Zellen, Chi-Quadrat) und kein Verlass auf Schönheit.
Der Seed je Schicht (11/23/37/53) macht die Wahl je Schicht unabhängig. Albedo, Normal und Maske einer Schicht lesen dasselbe Gitter mit demselben Seed und ziehen
damit **dieselbe** Variante (sonst wäre die Normal-Map eine andere Steinfläche als die Albedo).

### 6.3 Was sich im Codegen ändert (Schritt 3)

- Die beiden Array-Bombing-Knoten (`TextureArrayBombSample`, `NormalMapArrayBombSample`, Emitter `emitBombNode`, `MaterialGraph.cpp:906`) bekommen einen neuen
  Eingangs-Pin **„Groups“** (Vorgabe 0 = aus). Bei 0 ist der erzeugte Text **Byte für Byte der heutige**, also bleiben alle bestehenden Graphen und ihre Pipeline-Caches gleich
  (dieselbe Regel wie bei `// heTexArrays`, Textur-Doku §8.1). Das Auto-Material setzt Groups = 4.
- Bei Groups > 0: Hilfsfunktionen `heBombTapV` / `heBombGridV`, die zusätzlich die Variantenbits der drei Zellen zurückgeben, und `V = textureSize(arr, 0).z / G` im Knoten.
  Die Variante je Tap ersetzt das heutige „eine Ebene `v_l` für alle drei Taps“ (`MaterialGraph.cpp:926-927`).
- **Die Rundungs- und Klemmregel bleibt:** `clamp(floor(s + 0.5), 0, Ebenen − 1)` für die Gruppe (halbe Rundung ist zwischen APIs verschieden, ein Index außerhalb ist in MSL undefiniert,
  Textur-Doku §8.1) und danach eine zweite Klemme des Endslices auf `depth − 1`.
- **Ohne Bombing** (Static Switch „Texture Bombing“ aus, die Negativkontrolle) liest das Material Variante 0: ein Slice je Pixel würde eine Naht erzeugen, ein Überblenden zweier Varianten
  kostete zusätzliche Zugriffe. Der Modus ist ein Debug-Modus, kein Qualitätsmodus.
- **Optional, ohne Texturzugriff:** eine großräumige Tönung (Albedo × (1 ± a · (fbm − 0.5)), Integer-Fbm wie `dirtFbm`) mit höchstens **2 neuen Parametern** („Variation Amount“, „Variation Scale“).
  Von den 3 freien Slots (16 − 13) bleibt dann einer. Ob die Tönung nach dem Bild nötig ist, entscheidet Schritt 3.

### 6.4 Parameter-Budget (Frage der Queen, Anfrage 144)

Die Frage: stößt 2.0 an dieselbe Grenze wie das Wasser-Material (Thema 206, 14/16 Slots; Thema 229 plant die Erhöhung)? **Nein, nicht mit diesem Entwurf.**

- Das Limit ist `kMatMaxParams = 16` (`MaterialGraph.h:533`, auf `origin/release/0.7.0` ebenfalls 16): ein `vec4` je benanntem Parameter im `HeParams`-Block (`paramSlot`, `MaterialGraph.cpp:581`,
  Deklaration `:1619`). Über dem Limit wird der Standardwert des Knotens als Literal eingebacken (kein Absturz, aber der Regler fehlt).
- Das Auto-Material belegt **13** (`kAutoLandscapeParamCount`; der Test `tests/test_material_graph.cpp:2557` verlangt `gen.params.size() == 13`, also keine versteckten Wetter- oder Hilfsslots),
  mit ausgeschaltetem Bombing 12. Frei sind **3**.
- Variation braucht **0** neue Parameter: „Groups“ ist eine Konstante am Pin, die Variantenzahl V kommt aus `textureSize`, die Wahl aus dem Hash. Die optionale Tönung braucht höchstens **2** (15 von 16).
  Die Schicht-Meshes sind `FoliageLayer`-Felder und gebrauchen gar keine Material-Parameter; ihre Materialien sind eingebautes PBR.
- Grenze bleibt also unberührt. Käme mehr als ein dritter zusätzlicher Regler dazu (etwa je Schicht ein Variantenanteil), wäre 2.0 an der Wand, und das Anheben von `kMatMaxParams`
  ist **eine Änderung am UBO-Layout aller fünf Backends** (`vec4 v[N]`, Pin-Tabellen, Root-Signature). Das gehört Thema 229 und wird hier **nicht** angefasst. Sollte Thema 229 anheben,
  entsteht für 2.0 weder ein Konflikt noch eine Pflicht.

---

## 7. BCn-Kompression

### 7.1 Format je Texturart

| Karte | Farbraum, Kanäle | Windows/Linux (D3D11/12, Vulkan, GL ≥ 4.2) | Apple, Metal | macOS GL 4.1 | Rückfall |
|---|---|---|---|---|---|
| Albedo | sRGB, RGB | **BC7** (`…_UNORM_SRGB`) | ASTC 4×4 sRGB | BC3, siehe 7.4 | BC3, dann RGBA8 |
| Normal | linear, **nur XY** | **BC5** (zwei Kanäle) | ASTC 4×4 (XY in R, G) | **BC5 / RGTC** (gemessen nutzbar) | BC7 mit Z, dann RGBA8 |
| Maske | linear, R = AO, G = Rauheit, B = Höhe | **BC7** | ASTC 4×4 | BC3 | RGBA8 |

### 7.2 Warum nicht BC4 für die Maske

BC4 speichert **einen** Kanal. Die gepackte Maske (Textur-Doku §3) trägt drei. Eine Aufteilung in BC5 (AO, Rauheit) plus BC4 (Höhe) bräuchte ein viertes Array in `heTexP3` und
fünf statt vier Lesungen je Tap, also mehr Zugriffe für dieselbe Speicherersparnis (BC7 mit allen drei Kanälen kostet dasselbe wie BC5). **BC4 wird trotzdem gebaut** (Encoder, Enum, Upload,
Rundreise-Test), weil Einzelkanal-Importe (Rauheit, Höhe, Alpha als eigene Textur, `TextureImporter`) davon profitieren und weil Schritt 2 „BC1/BC3/BC4/BC5/BC7“ liefern soll.
Das Auto-Material liest BC4 nicht.

### 7.3 Normalen: Z wird rekonstruiert (eine Entscheidung für alle Backends)

BC5 hat zwei Kanäle. Der Normal-Map-Codegen liest heute `.xyz * 2.0 - 1.0` (`emitBombNode`, `MaterialGraph.cpp:953`; *Normal Map Array* und plain `:867`, `:893`), mit BC5 wäre Z null.
**Entscheidung:** Das Normal-Array speichert auf **allen** Backends und in allen Formaten nur XY in R und G; die Knoten rekonstruieren
`z = sqrt(saturate(1 - dot(xy, xy)))` hinter dem Lesen (beim Bombing je Tap vor der Drehung, Z bleibt von der Drehung unberührt). Damit gibt es **einen** Shaderpfad ohne
Formatpermutation, und Metal (ASTC) und D3D (BC5) laufen nicht auseinander. Neues Verhalten nur über ein Knotenfeld, das das Auto-Material setzt; bestehende Graphen
bleiben Byte für Byte gleich. Vorhandene RGBA8-Normal-Arrays mit gespeichertem Z lesen sich weiter richtig (XY reicht).

### 7.4 Neue Arbeit (BC4/BC5 sind neu, BC7/BC3 und ASTC vorhanden)

1. **Enum** (`Assets.h:581`) um BC1, BC4, BC5 *anhängen* (der Wert steht im TXMI-Byte auf der Platte). Neue Funktion `textureFormatBlockBytes()` (8 für BC1/BC4, sonst 16);
   `textureFormatIsBlock4x4` (`:584`) bleibt die Blockprüfung. Alle Stellen mit fester 16-Byte-Annahme anfassen: `cookTexture` (`HpakWriter.cpp:703`), die fünf Upload-Pfade (§2.3), `textureArraySliceBytes`.
2. **Encoder-Wrapper** neben `encodeBc3` (`HpakWriter.cpp:137`): `encodeBc1`, `encodeBc4`, `encodeBc5` über `stb_compress_dxt_block(alpha = 0)`, `stb_compress_bc4_block`,
   `stb_compress_bc5_block` (`stb_dxt.h:59-60`). BC5 liest R und G des RGBA8-Mips, BC4 den gewählten Kanal. Die Qualitätsstufen von `textureQuality` bleiben (stb hat nur normal/high).
3. **Arrays kochen:** `cookTexture` gibt für `layers > 1` heute `{}` zurück (`:730-735`). Neu: je Slice und je Mip kodieren, Slice-Mips nicht neu backen (`buildTextureArray` hat sie schon),
   `layers` im TXMI-Tail erhalten. `textureArrayPayloadValid` (`TextureArrayBuild.cpp:26`) akzeptiert Blockformate mit der richtigen Byte-Summe.
4. **Upload je Backend:** die vier „RGBA8 only“-Wächter aus §2.2 entfernen und die Formate abbilden:
   - **D3D11 / D3D12:** `DXGI_FORMAT_BC1/BC4/BC5_UNORM` (+ `_SRGB` für BC1), Arrays über dieselbe Subresource-Reihenfolge (`D3D11Renderer.cpp:4419`, `D3D12Renderer.cpp:3926`).
   - **Vulkan:** `VK_FORMAT_BC1_RGB_*`, `BC4_UNORM_BLOCK`, `BC5_UNORM_BLOCK` neben `BC7/BC3` (`VulkanRenderer.cpp:7022`), mit dem vorhandenen `vkGetPhysicalDeviceFormatProperties`-Wächter.
   - **OpenGL:** `GL_COMPRESSED_RED_RGTC1` / `GL_COMPRESSED_RG_RGTC2` (Tokens in `src/HE_Rendering/glad/include/glad/glad.h:786-789`), `glCompressedTexImage3D` für Arrays; neuer Wächter
     `glSupportsRgtc()` (RGTC ist Kern ab GL 3.0, daneben `GL_ARB_texture_compression_rgtc`) neben `glSupportsBptc` (`OpenGLRenderer.cpp:8153`).
   - **Metal:** `MTLPixelFormatBC4_RUnorm`, `BC5_RGUnorm` neben BC7/BC3 (`MetalRenderer.mm:9197-9209`), geschützt durch `supportsBCTextureCompression`.
5. **Tests ohne GPU:** Rundreise je Format (kodieren, in der Testkopie dekodieren, Toleranz; ein BC3-Dekodierer und ASTC-Rundreisen stehen schon in `tests/test_hpak.cpp:659`, `:781`), Byte-Summen der
   Array-Ebenen, Cook-Rückfall auf RGBA8 bei fehlendem Encoder. Bild-Tests liefern die CI-Jobs (§9).

Gemessen auf dem M5, damit die Apple-Zeile nicht geraten ist (Wegwerfprogramm, Metal und CGL, nicht im Repo):

| Abfrage | Ergebnis |
|---|---|
| Metal `supportsBCTextureCompression` | **1** (auf diesem Gerät; andere Macs *nicht geprüft*) |
| Metal `supportsFamily:Apple2` (ASTC) | 1 |
| GL `GL_ARB_texture_compression_bptc` (BC7) | **0**, also kein BC7 auf Apple-GL |
| GL `GL_EXT_texture_compression_s3tc` (BC1/BC3) | 1 |
| GL RGTC (BC4/BC5): Erweiterung in der Liste | 0 (Kernfunktion wird nicht gelistet) |
| GL RGTC: `glCompressedTexImage3D` mit `RG_RGTC2` und `RED_RGTC1` auf `GL_TEXTURE_2D_ARRAY` | **Fehler 0**, funktioniert |
| GL `GL_EXT_texture_sRGB` | 0 |

Letzte Zeile ist ein Risiko, das schon vor diesem Thema besteht: der BC3-Pfad auf Apple-GL nutzt `GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT` (`OpenGLRenderer.cpp:8128-8132`, `:8203-8205`).
Ob dieses sRGB-Format dort wirklich angenommen wird, ist **nicht geprüft** und gehört in die Probe von Schritt 2 (sonst Albedo auf Apple-GL als RGBA8 oder mit Dekodierung im Shader).

### 7.5 Wo komprimiert wird (die zentrale Entscheidung D6)

Die Engine-Arrays liegen als lose `.hasset` in `EditorDeps/EngineContent` (Platzhalter eingecheckt, echte 2K-Arrays über den EngineContent-Server, nie in git, Textur-Doku §4.4) und
werden im Editor als RGBA8 geladen. Ein Export-Cook allein hilft dem Editor also nicht.

| Weg | Inhalt | Urteil |
|---|---|---|
| A: nur beim Export (HpakWriter kocht Arrays je Slice) | Editor bleibt RGBA8, V = 3 in 2K sind 832 MiB; ein 1K-Satz für den Editor wäre 208 MiB | als **zusätzlicher** Weg nötig (ein Spiel exportiert auch Projekt-Arrays), als Editor-Lösung nicht |
| **B: komprimierte Assets** (`landscape_tex_gen --cook bc\|astc`) | Das Werkzeug schreibt die Arrays direkt komprimiert; je Format-Familie ein **Geschwister-Asset** mit eigener fester UUID (hinter `0x412`, dem Material). Der Renderer wählt beim Auflösen des Pfads nach Fähigkeit (Muster des `#arr`-Schlüssels, `ResolveGraphTexture`); die 128²-RGBA8-Platzhalter bleiben als universeller Rückfall und für die CI | **Empfehlung** |
| C: Transkodieren beim Laden in einen Cache | BC7 von 39 Slices in 2K, Kosten **nicht gemessen** | verworfen, bis jemand misst; BC7 ist der langsame Encoder |

B setzt voraus, dass Auflösung und EngineContent-Manifest (`src/HE_ContentSync`, Cache-Ordner `EngineContentCache`, `src/HE_Core/src/Diagnostics/GlobalState.cpp:135`) Geschwister kennen. Ob das Manifest nach Plattform schlüsseln kann, ist
**nicht geprüft** und die wichtigste offene Umsetzungsfrage für Schritt 2. Fällt es zu schwer aus, ist die Rückfallebene: nur den **BC-Satz** ausliefern (Windows/Linux, Intel-Macs) und Apple-Silicon
auf RGBA8 laufen lassen (genug Speicher bei Unified Memory), bis ein ASTC-Satz folgt. Die bestehende Einschränkung „ein Pak trägt ein Format je Textur“ (Apple-Silicon: Metal ASTC, GL BC3
nicht gleichzeitig) bleibt für Exporte bestehen und ist hier nicht gelöst.

---

## 8. Layer-Meshes

### 8.1 Kein zweiter Pfad

Jede Schicht-Mesh-Sorte ist eine **`FoliageLayer`** (`R:FoliageComponent.h:35`) an der Terrain-Entity. Streuen, Cluster, LOD, Schatten- und AO-Flags, Collab-Maske und Serialisierung sind da und
getestet; Thema 182 hat sie ausdrücklich für Thema 180 freigehalten. Ein eigener Streu- oder Zeichenpfad wird **nicht** gebaut.

### 8.2 Welche Meshes (prozedural, aus `mesh_gen`)

Die Meshes sind **zur Werkzeug-Zeit** erzeugte Engine-Assets, nicht zur Laufzeit: gleiche Bytes auf fünf Backends, cookbar, mit handgemachten LOD-Stufen (die Engine hat keinen Simplifier,
`R:docs/foliage-instancing-lod-plan-2026-10-10.md` §0 Punkt 3). Ort: `src/HE_Tools/src/MeshGen/main.cpp` (Primitive `Cube`, `Sphere`, `Cylinder`, `Cone`, `Plane`, `Capsule`, `Torus` stehen in der Tabelle
um `:356`); die UUIDs (`hi = 0x100 + Eintragsindex`, nur anhängen) laufen bis `0x108` (Kreuz-Karte, `R:DefaultAssets.h:104`), neue Meshes ab `0x109`, mit `static_assert` im Generator.

| Schicht | Mesh-Sorte | Erzeugung | LOD-Stufen |
|---|---|---|---|
| Gras | Büschel (6 bis 12 gebogene Klingen-Quads, beidseitig) | deterministische Biegung und Breite aus Seed | 3 (Klingenzahl halbiert), letzte Stufe Kreuz-Karte nur mit eigenem Material |
| Erde | Kiesel (abgerundeter Low-Poly-Körper) | Ikosphäre, Rauschverschiebung, abgeflacht | 2 |
| Fels | Felsbrocken (mehrere Formen) | Ikosphäre, mehrstufiges Rauschen, ebene Schnittflächen | 3 |
| Schnee | niedrige Wehe / Eisbrocken | Kuppel mit Rauschen | 2 |

Formen-Varianten (Fels A/B/C) sind **weitere Schichten** mit demselben Muster, nicht ein Mesh mit Varianten. `kMaxLayers = 32` reicht. Impostor-Baking gibt es nicht (nicht gebaut,
`R:…plan…` §16.3); die Kreuz-Karte hilft Gras nur mit einer Kartentextur, die das Backen (Teil 3c) bräuchte. Für 2.0 gilt: **LOD = Mesh-Stufen + Dichte-Ausdünnung.**

### 8.3 Platzierung: Regeln als Felder an `FoliageLayer`

Das Auto-Material verteilt die Schichten **im Pixel-Shader** (Hang, Höhe, Rauschen). Die Meshes müssen dort stehen, wo die Schicht sichtbar ist, also braucht die CPU-Streuung dieselben Regeln.
Neue Felder an `FoliageLayer`, jedes nur dann serialisiert, wenn nicht Vorgabe (`R:SceneSerializer.cpp` `foliageLayerToJson` `:166`, damit alte Szenen byte-gleich bleiben):

- `slopeMin`, `slopeMax` (gleiche Größe wie das Material: `1 − N.y`),
- `heightMin`, `heightMax` (Welt-Y),
- `surface` (Beliebig / Gras / Erde / Fels / Schnee) mit Schwelle: wählt die Oberfläche, die das Material an dieser Stelle zeigt,
- optional ein Paint-Schicht-Gewicht (Weightmap-Kanal), wenn von Hand gemalt werden soll.

Umsetzung in `scatterLayer` (`R:FoliageSystem.cpp:140`): wie die Dichtemaske **per Zurückweisung** (ein Kandidat bleibt mit der Regelwahrscheinlichkeit; die Auswürfelung kommt aus einem eigenen
Hash je Kandidat, damit Überlebende ihre Lage behalten). Eingänge: Höhe aus `terrainHeightAt` (`src/HE_Scene/src/TerrainMeshGenerator.cpp:180`), Normale aus Differenzen davon.
**Rock und Schnee** sind analytisch (Hang, Höhe) und stimmen mit dem Shader überein. **Erde** ist ein Rauschfeld: dafür braucht die CPU einen Zwilling von `heHashI2` / `heFbmI` (`MaterialGraph.cpp:1727`).
Das ist machbar, weil es ein **Integer**-Hash ist und beide Seiten dasselbe Ergebnis geben müssen. **Pfützen sind ausgenommen** (Wasser ist nicht Teil des Themas).
Die Werte stehen absolut an der Schicht (kein Zugriff aufs Material beim Streuen); ein Knopf im Inspector kopiert sie aus den Parametern der Material-Instanz („Match Auto Material“).
Reicht ein Skalar nicht und braucht eine Regel ein Raster, folgt sie dem Masken-Muster der Collab-Synchronisation (`TerrainSync`, Feld 16 + i, laut Notiz von Thema 182).

### 8.4 Anbindung ans Instancing

Der Weg einer Layer-Mesh-Instanz ist der der Foliage-Instanzen (Abschnitt 2.4):
`scatterLayer` → `FoliageStore` → `extractFoliage` (Cluster je Bucket und LOD-Stufe) → `GeometryPass::execute` / `RenderSorter::batchDepthRuns` entfalten über `appendInstances` →
`DrawCall::instanceTransforms` → der Instanz-Speicher des Backends (Tabelle 2.4) → **ein Draw je Lauf gleichen Meshes und Materials**, geschnitten bei 65 536 Instanzen.
Es entsteht kein neues Backend-Stück. Draw-Anzahl ≈ (Mesh × Material × LOD-Stufe) und nicht ≈ Pflanzenzahl; so wird sie in Schritt 5 belegt
(Zähler `m_counters.draws`, die Zeile `HE_FOLIAGE_STATS` aus `R:FoliageExtract.cpp:40`, Skript `R:scripts/perf/foliage_graph_ab.py`).

### 8.5 Material und Schatten der Meshes

- **Zuerst eingebautes PBR** (eine Farbe je Schicht nahe der mittleren Albedo, Rauheit passend): läuft überall auf dem Instanzpfad, auch im exportierten Spiel. Ein Graph-Material mit
  den Schicht-Arrays (Weltraum-UV, gleiche Variante wie der Boden) wäre schöner, **schleift im exportierten Forward-Spiel weiter** (`R:…plan…` §15.2) bis die Queen Anfrage 105 entscheidet.
  Das ist ein Folgeschritt, nicht Teil des Solls.
- Schatten und AO nach dem Budget des Foliage-Plans (§7.5): Kiesel und Büschel `castsShadow = false`, `contributesAO = false`; Felsbrocken werfen Schatten mit `shadowDistance`.
  Cluster gehen nicht in die GI-Instanzlisten (Folge: unter Felsbrocken kein GI-Schatten, bewusst).
- Die Meshes schneiden das Gelände (Fuß im Boden), `bucketSize` bleibt bei 32 m.

---

## 9. Plan je Backend

„Lokal belegt“ heißt: auf diesem Mac (Apple M5) gebaut und gerendert. „Offen“ ist, was Hardware oder CI braucht. **Auf dem Mac wird nichts von D3D oder Vulkan gerendert**; dort gelten nur Syntax
und Baupfad (`-fsyntax-only` gegen MoltenVK für Vulkan, HLSL-/SPIR-V-Kreuzübersetzung in `tests/test_material_graph.cpp`).

| Backend | Varianten (Arrays, Codegen) | BCn | Layer-Meshes / Instancing | Lokal belegt | Offen (Hardware / CI) |
|---|---|---|---|---|---|
| **Metal** | `texture2d_array`, `V` aus `get_array_size()`; Pin-Tabelle unverändert | ASTC (Albedo, Normal XY, Maske), BC4/5/7 nur hinter `supportsBCTextureCompression` (hier = 1) | Puffer 5, transient | Bild gegen OpenGL (`he_shot.py`), Zähler `draws`, Metal-Kompilierung der MSL, Tests | ASTC-Qualität im Fenster, andere Macs ohne BC |
| **OpenGL** (4.1 auf dem Mac) | `sampler2DArray`, Array-Grenze gemessen 2048 | Mac: BC5 (RGTC) für Normal, BC3 für Albedo/Maske, **kein BC7**; Windows/Linux GL ≥ 4.2: BPTC, neuer `glSupportsRgtc` | `m_instanceVBO`, Attribute 4 bis 7 | Bild, Zähler, Upload der BC5/BC3-Arrays (Fehler 0 gemessen), Tests | BPTC-Pfad (Windows/Linux-Treiber), sRGB-S3TC auf Apple-GL |
| **D3D11** | `Texture2DArray`-SRV, FXC SM5.0 16/16 unverändert | BC7/BC5/BC1/BC4 (`DXGI_FORMAT_*`), Subresource je Ebene | `instanceSB` + `ensureMatInstVB` | HLSL-Kreuzübersetzung, Syntax | **Windows-CI** (FXC-Sweep, WARP-Pixeltest im `he_tests`, `vulkan-lavapipe` n. a.), **NN-WS03-Hardware-Smoke offen** |
| **D3D12** | wie D3D11, Root-Signatur deckt die Slots (`D3D12MaterialRootSignature.h`) | wie D3D11, `GetCopyableFootprints` je Ebene | Instanz-Arena (`allocInstances`) | HLSL-Kreuzübersetzung, Syntax | Windows-CI (PSO gegen Root-Signatur auf WARP), Hardware-Smoke offen |
| **Vulkan** | `VK_IMAGE_VIEW_TYPE_2D_ARRAY`, 17 Sampler/Weightmap-Wächter unverändert | `VK_FORMAT_BC*_BLOCK`, Format-Eigenschaften-Wächter | Instanz-Arena (`allocInstances`) | SPIR-V-Kreuzübersetzung, `-fsyntax-only` gegen MoltenVK (mit Negativkontrolle) | CI-Job **`vulkan-lavapipe`** (`.github/workflows/ci.yml:547`, Bild-Tests auf der CPU), Hardware-Smoke offen |

Der Bildvergleich der Backends nutzt die Toleranz aus der Textur-Doku §7.5 (lit, mean|Δ| ≤ 1,0 und ≤ 0,5 % Pixel mit |Δ| > 8). BCn-Bilder weichen von RGBA8 ab (Verlust), Backends **untereinander**
müssen gleiche Blöcke dekodieren; verglichen wird deshalb immer „gleiches Asset, gleiches Format“, nie komprimiert gegen unkomprimiert.

---

## 10. Zuordnung zu den Schritten

| Schritt | Trägt aus diesem Entwurf | Stand (Schritt 11, 2026-10-21) |
|---|---|---|
| Vorab (Queen) | Merge von `origin/release/0.7.0` (D8), Konflikt in `tests/test_material_graph.cpp` | erledigt (`e6d4920d`, Schritt 9) |
| 2: BCn-Encoder und Upload | §7.4, §7.5 (Entscheidung D6, Manifest-Frage), Tests ohne GPU, Probe Apple-GL sRGB und kleine Mips | Schritt 2 abgebrochen, in Schritt 10 neu gemacht: BC1/BC4/BC5, Encoder, Array-Cooking (nur mit `cookTextureArrays`), Decoder, Upload auf fünf Backends. Route B (§7.5, Geschwister-Assets je Familie) und Format je Karte **nicht** gebaut |
| 3: mehr Texturen und Variation | §5, §6 (Groups-Pin, Variantenbits, Z-Rekonstruktion §7.3), Arrays V = 3 aus den Quellen (§12) | Groups-Pin, `heBombGridV`, 4V+1-Arrays und Packer gebaut; echte V = 3-Fotos nicht gepackt. Z-Rekonstruktion (§7.3) nicht gebaut |
| 4: Meshes erzeugen und platzieren | §8.2, §8.3 (`mesh_gen`, `FoliageLayer`-Felder, CPU-Zwilling) | Regeln (Hang, Höhe, Malgewicht) und 16 Meshes gebaut; `surface`-Regel und CPU-Rauschzwilling **nicht** |
| 5: Instancing anbinden | §8.4, §8.5, Draw-Zahl belegen | kein Backend-Code nötig (Cluster-Pfad trägt es); Draw-Zahl flach auf Metal und OpenGL belegt; D3D/Vulkan nur über CI; Meshes ohne eigenes Material (§8.5 offen) |
| 6: Audit | `scripts/he_perf_capture.py`, `scripts/he_perf_tables.py`, Muster `docs/perf-audit/*` | Metal-Audit mit Messwerten: `docs/perf-audit/auto-landscape-material-2-audit-2026-10-21.md`; D3D/Vulkan-Zeiten und OpenGL-Live-Capture offen |
| 7/11: Verifikation | voller Build und Tests im Vordergrund, Doku nachziehen | `docs/auto-landscape-material-2-verification-2026-10-21.md` |

---

## 11. Offen, nicht geprüft, Annahmen

- **Nicht gegen einen Header geprüft:** D3D11/D3D12-Array-Grenze (2048), Vulkan `maxImageArrayLayers`, Metal-Array-Länge; lavapipe-Grenzen (Ebenen und Sampler).
- **Gemessen nur auf einem Gerät** (M5): Metal `supportsBCTextureCompression = 1`, GL ohne BPTC, RGTC nutzbar, Ebenen 2048, 16 Fragment-Einheiten. Andere Macs, Windows/Linux-Treiber: nicht geprüft.
- **Apple-GL `GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT`** (7.4): vorbestehendes, ungeprüftes Risiko.
- **Manifest nach Plattform** (7.5): nicht geprüft, hängt an `src/HE_ContentSync`.
- **Kosten des BC7-Encoders** für 39 Slices in 2K: nicht gemessen.
- **Textur-Doku, Zahl der Zugriffe (30 gegen 36):** Schritt 3 zählt nach.
- **Kleine Mips in Blockformaten** auf allen fünf Backends: zu prüfen (4).
- **Graph-Material-Meshes im exportierten Forward-Spiel** schleifen (Anfrage 105 offen).
- **Die GI-Näherung** kann Texturknoten nicht falten (`approxFoldNode`, Textur-Doku §10.5); Variantentexturen ändern daran nichts. Layer-Meshes sind aus der GI ausgenommen.
- **CI auf diesem Zweig:** der rote Lauf 38038013020 (auf `7b8a1b9b`, vor dem Merge) ist überholt. Lauf 39548430947 auf `fbc999fd` ist grün (Windows, macOS, Linux, Linux · Vulkan (lavapipe)); `ab87f6cb` danach ändert nur ein Audit-Dokument. Dass das Windows-Layer-Mesh-Bildtest-Schritt D3D11/D3D12 wirklich gerendert hat, ist daraus **nicht** ablesbar (der Schritt ist `continue-on-error`; Artefakt `layer-mesh-shots-windows` nicht angesehen). **Hardware-Smoke-Test auf NN-WS03: offen.**
- **Stand Schritte 2-10:** siehe die Spalte „Stand“ in §10 und die Verifikation unter `docs/auto-landscape-material-2-verification-2026-10-21.md`.
- **Zeilennummern** der Textur-Doku (§8.2 `:8920`, `:5895` usw.) sind veraltet; diese Datei nennt die aktuellen.

---

## 12. Quelle der zusätzlichen Texturen

Die zusätzlichen Albedo-, Normal- und Maskenvarianten (Gras, Erde, Fels, Schnee) kommen von **Poly Haven** (`polyhaven.com`, **CC0**, API `https://api.polyhaven.com`). Die Lizenz ist unkritisch (CC0), die Quelle je Schicht
wird beim Einkauf notiert. Der Weg existiert: `scripts/landscape-textures/stage_polyhaven.py` (EXR und 4K → 2K-PNG) und `landscape_tex_gen --pack` / `--arrays-only` (Textur-Doku §4.4, §14).
**In diesem Schritt wird nichts heruntergeladen und nichts committet**; die großen Dateien gehören nicht in git (Textur-Doku §4.4, kein LFS). Der Download ist ein eigener, späterer Schritt (Schritt 3).
