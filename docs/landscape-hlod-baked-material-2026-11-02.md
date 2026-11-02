# Landscape-HLOD mit gebackenem Material (Thema 228, Mac/Metal umgesetzt; Quadtree von Thema 326 später)

**Stand 2026-11-02 (Abschnitt 7):** Das gebackene Fernmaterial ist für die **heutigen Terrain-Chunks** gebaut, im Export automatisch, auf Metal. Abschnitte 1 bis 6 sind der ursprüngliche Entwurf und die Messungen davor; was daraus
Wirklichkeit wurde, steht in Abschnitt 7 (mit Messwerten und dem, was nicht geprüft ist).

Auftrag (Nutzer, 2026-11-02, im Rahmen von Thema 228): Die fernen Stufen der Landschaft sollen **das Ergebnis des Landscape-Materials gebacken bekommen**, damit dafür zur Laufzeit keine Berechnung anfällt,
**mit automatischem Downscaling bei zu hoher Auflösung**.

Entscheidungen des Nutzers (Rückfrage am selben Tag):
1. **Ziel:** erst der Quadtree aus Thema 326. Jetzt wird nur das Format und die Regel entworfen; gebaut wird, wenn der Quadtree-Renderer steht.
2. **Zeitpunkt:** **automatisch beim Export** (keine Editor-Aktion, keine Laufzeit-Berechnung).
3. **Inhalt:** **Albedo + Normal + Roughness**.

## 1. Bestand

* 0.8.0: Ein Terrain-Chunk hat Meshes in mehreren LOD-Stufen (`TerrainSystem.cpp` ~500-560, `LODComponent`), alle mit **demselben** Landscape-Material. Eine LOD-Stufe mit eigenem Material gibt es nicht.
* Thema 326 (Zweig `claude/landscape-weltmassstab-100km`): `LandscapeTile` (512-m-Kacheln, Höhen und Gewichte als Pyramide bis 128 m Raster), `LandscapeStore`, Renderer-Teil B. Der Plan sieht den Terrain-HLOD als **die Quadtree-Stufen selbst**
  vor (`docs/landscape-world-scale-plan-2026-10-28.md`, Punkt 7a) und kennt kein gebackenes Material für die Ferne. Dieser Entwurf ergänzt es.

## 2. Was gebacken wird und womit

Der G-Buffer-Fragment-Shader des Materials (`MaterialAsset::customShaderGBufGlsl`, aus `MatShaderGen::glslGBuffer`) schreibt schon genau die drei Dinge:
`oGB0 = (BaseColor.rgb, Metallic)`, `oGB1 = (oktaedrisch kodierte Normale xy, Roughness, Specular)`. **Der Bake ist eine Aufnahme dieses G-Buffers**, kein neuer Shader:

* je Kachel und Stufe ein senkrechter, orthografischer Render der Kachel (Höhenfeld + Gewichte) mit dem echten Landscape-Material in den G-Buffer-Pass,
* die Texel auslesen: Albedo (sRGB), Normale (zwei Kanäle, Welt-Raum, aus der Oktaeder-Kodierung zurück), Roughness (ein Kanal),
* mit 2× Überabtastung rendern und mit einer Box auf die Zielgröße mitteln (kein Aliasing, kein Schimmern in der Ferne),
* ein Rand von einem Texel aus der Nachbarkachel, damit Filter und Mips an der Kachelgrenze keine Naht zeichnen.

Die Zielformate sind die der anderen Texturen (ASTC 4×4 auf Metal, BC7/BC3 anderswo; Normale als zwei Kanäle, Roughness einkanalig, wo das Format es zulässt). Die gebackenen Kacheln liegen als **Pyramide parallel zu den Gewichten**
(gleiche Kachel, gleiche Stufen) und werden wie die Gewichte als Atlas/Array gestreamt, sodass der Renderer sie mit demselben Kachel-Index lädt.

**Eingefroren beim Backen** (steht nicht im Bild, wenn es sich zur Laufzeit ändert): Wetter und Nässe, Schnee-/Sand-Auflage aus dem Wetter, Wind, Spuren der Trail-Clipmap, Licht und Zeit. Der Bake läuft mit trockenem, unbeleuchtetem Zustand; die ferne Stufe bekommt Licht,
Schatten und Nebel wie jedes andere Objekt, und wo das Material wetterabhängig ist, kann die Ferne eine billige globale Tönung dazu bekommen. Das ist ein bewusster Verlust und muss im Editor sichtbar sein
(Vorschau "Fern-Stufe" neben dem echten Material).

## 3. Automatisches Downscaling

`HE::landscape::planHlodBake` (`src/HE_Scene/include/HorizonScene/LandscapeHlodPlan.h`, getestet in `tests/test_landscape_hlod_plan.cpp`, reine Arithmetik, keine Abhängigkeit zu den Dateien von Thema 326):

* **Nötige Auflösung:** Ein Texel soll höchstens so grob sein wie ein Bildschirmpixel dort, wo seine Stufe übernimmt. Ein Pixel in der Entfernung d spannt `2·d·tan(fovY/2) / Bildhöhe` Meter; Stufe i beginnt bei `nearM · growth^i`
  und braucht `tileM / Raster` Texel je Kachelseite, aufgerundet auf eine Zweierpotenz. Beispiel (60°, 1080p, 512-m-Kachel, Übergang bei 2 km): 240 Texel, also 256 (Raster 2 m); die Stufen darüber 128, 64, 32, 16, 16.
* **Deckel je Kachel** (`maxTexels`, Vorgabe 256, Boden `minTexels` 16): Was darüber läge, wird heruntergerechnet (`downscaled`).
* **Budget:** Passt die ganze Welt nicht in `budgetBytes`, wird die feinste Stufe halbiert, die gröberen folgen (eine gröbere Stufe hat nie mehr als halb so viele Texel), bis es passt oder der Boden erreicht ist; geht es auch dann nicht, steht `withinBudget = false` im Plan.

Rechnung mit der Vorgabe (3 Karten, 8 Bit je Texel, Mips): **350 KB je Kachel** (350 208 Byte) bei voller Auflösung.

| Welt | Kacheln | volle Auflösung (2 m Raster) | 4 GB | 2 GB | 500 MB |
|---|---|---|---|---|---|
| 4 km | 8 × 8 | 22,4 MB | passt | passt | passt |
| 100 km | 195 × 195 | **13,3 GB** | 4 m (128 Texel) | 8 m (64) | 16 m (32) |

Bytes vor der zstd-Kompression des Paks (Texturen daraus komprimieren bei Fotos rund 46 %; für gebackene, glatte Landschaft nicht gemessen). Für eine 100-km-Welt ist das Downscaling also keine Randnotiz, sondern die Regel:
ein 500-MB-Budget heißt 16 m Raster auf der feinsten gebackenen Stufe, und das echte Material muss bis zu dem Radius reichen, in dem ein Pixel größer ist (hier rund 15 km bei 1080p). **Der Übergangsradius ist deshalb ein Regler
im Projekt**, und der Editor sollte beim Export sagen, welches Raster er je Stufe erreicht hat und welche Stufen herunterskaliert wurden.

## 3a. Messungen (2026-11-02, MacBook M5; was gemessen und was nur abgeleitet ist)

**Encode-Durchsatz, gemessen** (`tests/test_bench_landscape_arrays.cpp`, die echten Landscape-Arrays, 5 × 2048² mit Mips = 28 Mio. Texel je Array): ASTC 4×4 Fast 0,14 / 0,62 / 0,07 s je Mio. Texel für Albedo / Normal / Mask (die Normal-Karte ist 4× langsamer
als die Mask-Karte bei gleicher Größe: der Encoder hängt stark vom Inhalt ab), BC7 0,15 bis 0,16, BC3 0,015 bis 0,017.

**Eine gebackene Kachel durch den echten Pak-Koch, gemessen** (neu: `tests/test_bench_hlod_tile.cpp`, nur mit `HE_BENCH_TILE_PNG`; Eingabe: ein Top-down-Render des echten `M_AutoLandscape` aus dem Witness `HE_DUMP_AUTOLAND`, Forest-Gruppe, auf 256 / 128 / 64 / 32 Texel
heruntergefiltert; drei Ausschnitte: gemischt, verrauschte Ebene mit Textur-Bombing, glatte Schnee-Hochfläche; je Zelle 5 Läufe, Median):

| Seite | roh (ASTC/BC7/BC3) | zstd-19 gespart, ASTC: verrauscht / gemischt / glatt | BC7: verrauscht / gemischt / glatt | ASTC Pack-Aufruf |
|---|---|---|---|---|
| 256 | 87 659 B | 18 % / 37 % / 63 % | 19 % / 49 % / 80 % | 11,4 bis 13,0 ms |
| 128 | 22 123 B | 12 % / 33 % / 57 % | 13 % / 45 % / 75 % | 7,4 bis 8,0 ms |
| 64 | 5 739 B | 6 % / 30 % / 51 % | 8 % / 40 % / 69 % | 5,9 bis 6,5 ms |
| 32 | 1 643 B | 4 % / 25 % / 41 % | 6 % / 28 % / 50 % | 4,8 bis 5,0 ms |

* Die rohe Größe stimmt mit dem Planer überein (65 536 Texel × 4/3 = 87 381 B plus rund 280 B Kopf).
* **Jeder Pack-Aufruf kostet fest 4 bis 5 ms** (ASTC-Kontext), auch bei 32². Der Durchsatz allein unterschätzt kleine Kacheln: Mit dem 500-MB-Budget (32², 16², …) sagte das Durchsatzmodell 1,6 Minuten für die Welt, mit 3 Aufrufen je Kachel
  (je einer pro Karte, die Pyramide ist die Mip-Kette dieses einen Texturaufrufs) sind es rund 3 × 5 ms × 38 025 = **9,5 Minuten**; jede Stufe einzeln zu kochen wären rund 78 Minuten. **Folgerung: je Karte eine Textur mit Mip-Kette, besser mehrere Kacheln
  in ein Array (`cookTextureArrays`), nie jede Stufe einzeln.**
* **Kompression:** Auf dem Witness spart zstd bei ASTC zwischen 4 und 63 %, je nach Inhalt (verrauschtes Textur-Bombing am wenigsten, glatte Fläche am meisten). Für die 13,3 GB roh bei voller Auflösung heißt das grob **4,9 bis 10,9 GB komprimiert**. Die BC3-Werte (bis 91 %) sind
  nicht übertragbar: Die Hälfte jedes BC3-Blocks ist ein Alpha-Block, der hier konstant ist; für deckendes Albedo wäre BC1 das richtige Format (nicht gemessen).
* **Vorbehalt:** Der Witness ist beleuchtet und stark verschneit, kein reines Albedo. Ein echter, unbeleuchteter Bake aus der G-Buffer-Aufnahme ist nicht gemessen; ob er besser oder schlechter komprimiert, weiß ich nicht.

**Render-Zeit, abgeleitet, nicht neu gemessen:** `docs/perf-audit/auto-landscape-material-2-audit-2026-10-21.md` misst den Scene-Pass des Auto-Landscape-Materials mit 11,4 bis 13,2 ms bei 2840 × 1528 (4,34 Mio. Pixel), also **2,6 bis 3,0 ns je Pixel**
(Vorbehalt der Quelle: bei billigen Frames taktet die GPU herunter und die Zeit ist aufgebläht). Eine Kachel des Plans "voll" hat mit 2 × 2 Überabtastung 350 208 Pixel über alle Stufen: **rund 1 ms GPU je Kachel**, für 38 025 Kacheln rund 40 s reine GPU-Zeit.
Dazu kommen Aufruf-, Auslese- und Filterkosten je Kachel, die nicht gemessen sind. **Der Engpass ist das Codieren (rund 23 bis 78 Minuten bei ASTC, je nach Aufrufstrategie), nicht das Rendern.**
Mein eigener Profiler-Lauf (`he_perf_capture.py`, Kopie von Catania, Kamera von oben) hat das Landscape nicht im Bild gehabt (Scene-Pass 0,23 ms, SSAO 5,5 ms) und ist deshalb nicht verwertet.

## 4. Ablauf beim Export (die Entscheidung "automatisch")

1. Der Export-Dialog baut seine Szenen auf dem Hauptthread (so heute). Dort kommt ein **Bake-Schritt** dazu, der die GPU des Editors benutzt, mit Fortschritt und Abbruch (Auftrag des Hauptthreads, nicht des Export-Threads).
2. Je Kachel und Stufe ein Zwischenergebnis, **zwischengespeichert nach einem Inhalts-Hash** von (Höhen, Gewichte, Material samt Texturen, Bake-Einstellungen). Ein zweiter Export bäckt nur Kacheln neu, die sich geändert haben. Ohne diesen Cache ist
   der Export einer 100-km-Welt nicht zumutbar.
3. Das Ergebnis geht als abgeleitete Assets (Ids aus der Kachel-Id berechnet, so wie beim Mesh-Textur-Bake, `Hpak/MeshBakePass`) in den Pack-Lauf; der Erreichbarkeitsfilter kennt sie.
4. Fehlt dem Export die GPU (Kommandozeile, `hpak_packer`), wird nicht gebacken; das Spiel fällt auf das echte Material zurück und der Export-Log sagt es.

## 5. Offene Punkte und Risiken

* **Renderer:** Ein senkrechter G-Buffer-Render einer Kachel braucht eine Bake-Funktion im `IRenderer` (oder einen Offscreen-Pfad) auf allen fünf Backends. **Deferred gibt es nur auf Metal und GL**; D3D11/D3D12/Vulkan brauchen für die
  Aufnahme einen MRT-Pass mit demselben Fragment (`customShaderGBufGlsl` lässt sich schon für alle Backends kreuzkompilieren). Auf dem Mac nicht prüfbar, gehört auf Windows.
* **Weltkoordinaten:** Material und Auto-Landscape nehmen heute eine Gewichts-UV über die ganze Landschaft an (Plan Punkt 7, "Weltkoordinaten-UVs"). Der Bake muss dieselben Weltkoordinaten bekommen wie der Renderer, sonst liegen Muster nicht auf der Kachelgrenze.
* **Zeit:** Nicht gemessen. Bei 38 025 Kacheln × 6 Stufen entscheidet die Zeit je Render (und der Cache) über die Brauchbarkeit; erste Zahl vor jeder Umsetzung.
* **Kachelnähte:** Rand von einem Texel, gleiche Höhen an den geteilten Kanten (das Tile-Format teilt sie schon), Normalen aus dem gemeinsamen Höhenfeld, nicht aus der Kachel allein.
* **Material-Eingaben, die sich zur Laufzeit ändern** (siehe Abschnitt 2): braucht eine Entscheidung, ob Wetter in der Ferne sichtbar sein muss.

## 6. Vorschlag für Thema 326

1. In Schritt 5 ("Entladen ganzer Regionen, Entity-Zellen und HLOD-Backen") einen Teil **"gebackenes Fernmaterial"** aufnehmen: Pyramide parallel zu den Gewichten, Streaming mit demselben Kachel-Index.
2. Beim Quadtree-Renderer (Schritt 2) ein **Fernmaterial** vorsehen: ab dem Übergangsradius statt des Landscape-Graphen ein Material, das die drei gebackenen Karten liest (drei Zugriffe, keine Layer-Mischung).
3. Zuerst die Zeit und das Raster an einer Kachel der Testwelt messen (Abschnitt 5), dann erst den Export-Schritt bauen.

## 7. Umgesetzt (2026-11-02, Metal, die heutigen Terrain-Chunks)

Die Rückfrage "erst Quadtree" hatte ich als "nur entwerfen" gelesen. Das war zu eng: der Bake selbst hängt nicht vom Quadtree ab. Gebaut ist er jetzt für die **Chunks von `TerrainSystem`** (ein Chunk = eine Kachel); die Bilder, der Planer und die
Ids tragen später unverändert zu den Kacheln von Thema 326 (`HlodBaker::bakeRect` nimmt jedes quadratische Weltrechteck).

**Abweichung vom Entwurf (Abschnitt 2/4):** aufgenommen wird nicht der G-Buffer, sondern drei **unbeleuchtete Ansichtsgraphen** desselben Auto-Landscape (`AutoLandscapeView::BaseColor`, `Normal` = Weltnormale·0,5+0,5, `Surface` = R AO, G Roughness),
senkrecht von oben, orthografisch, im Vorwärtspfad. Grund: ein unbeleuchteter Ausgang ist bytegenau prüfbar (Konstante rein, Konstante raus), der G-Buffer-Pfad existiert nur auf Metal und GL.

| Baustein | Wo |
|---|---|
| Rohausgabe des Renderers: `EditorCameraOverride::rawOutput` (+ `rawSrgb` für das Albedo) in `RenderSceneImage`, nur Metal: kein Tonemap, Bloom, SSAO, AA, DoF, SSR, GI, Vorwärtspfad | `IRenderer.h`, `MetalRenderer.mm` (`kTonemapMSL`, `RenderSceneImage`) |
| `HlodBaker`: Szenenkopie **einer** Landschaft (ihre Material-Overrides und Static Switches werden auf die Ansichtsgraphen übertragen), Kamera je Chunk, Warten auf die drei Pipelines über `PumpShaderBuilds`, Box-Filter von `side·ss` | `HE_Scene/LandscapeHlodBaker.h/.cpp` |
| `buildHlodAssets`: drei einstufige RGBA8-Texturen (Albedo sRGB) + **ein** Material, das sie über die Weltposition liest; Ids/Pfade aus (Terrain-Entity-Id, cx, cz) unter `Baked/Landscape/` | `HE_Scene/LandscapeHlod.h/.cpp` |
| `LODLevel::materialId`; `TerrainSystem` setzt es für die Stufen ab 1, wenn das Material resident ist (Fehlgriff = ein Hash-Lookup); `RenderExtractor` nimmt es für die gewählte Stufe | `LODComponent.h`, `TerrainSystem.cpp`, `RenderExtractor.cpp` (`lodLevelMaterial`) |
| Export: `bakeWorldLandscapes` (Planer `planHlodBake`, Budget, Abbruch), `PackSettings/ExportSettings::extraAssets` (Ordner = Reach-Wurzel), Export-Dialog "Bake far landscape" + Budget (MB), im Profil gespeichert | `LandscapeHlod.cpp`, `HpakWriter.cpp`, `ExportDialogPanel.cpp`, `ProjectManager.*` |

**Gemessen (Mac, Metal, 2026-11-02):**

* Rohausgabe: eine unbeleuchtete Konstante (0,2 / 0,4 / 0,6) kommt als **51 102 153 in allen 4096 Pixeln** zurück (`HE_DUMP_RAWTEST`), sRGB-kodiert als 124 170 203; derselbe Frame ohne Rohmodus: 29 33 50.
* Platzierung: vier Chunks einzeln gebacken und zusammengesetzt gegen die ganze Landschaft in einem Bild: mittlere Abweichung 0,32/255 (Albedo), 0,000 (Normal), 0,11 (Surface) (`HE_DUMP_HLODMOSAIC`). Dazu liegen unsymmetrische Texturmuster im Fernbild an denselben Stellen wie im echten Material, also stimmen beide Achsen.
* Echte Landschaft (Kopie von Catania, 2 km, 256 Chunks à 125 m, Mountains): **alle 256 Chunks in 3,0 bis 3,3 s** headless gebacken, 256 px je Chunk (0,49 m je Texel, vom Planer: "was der Schirm will" ab 750 m, bei der Testeinstellung 150 m "unter dem, was der Schirm will", weil bei 256 gedeckelt). 1024 Assets.
* Pak (echter Export über `ProjectExporter`, mit Erreichbarkeitsfilter): **95,9 MB → 164,9 MB (+69,0 MB)**, geplant 64 MiB (67,1 MB). Export 23,6 s → 30,1 s.
* Im exportierten Spiel (aus dem Pak): "256 with a baked far material" im Terrain-Log, 0 Fehler. Mit der Landschaft auf Bodenhöhe ist der Unterschied zum echten Material klein (mittlere Abweichung 0,022/255, höchstens 29), weil alles jenseits von 150 m auf Augenhöhe in einem schmalen Streifen am Horizont liegt.
* **GPU-Zeit** (Catania-Kopie, Kamera 150 m hoch quer über die Landschaft, ferne Stufen ab 150 m, je drei abwechselnde Läufe, `he_perf_capture.py`, vsync aus): Szenen-Pass p50 **12,03 → 10,61 ms**, GPU-Frame 12,81 → 11,42 ms, **81,6 → 92,2 FPS**; gleiche 220 Draws, gleiche 59 597 Dreiecke; Streuung unter 0,05 ms.

**Fallen, die mich Zeit gekostet haben (alle behoben, je mit Test oder Beleg):**

1. `RenderSceneImage` zeigt ein frisch registriertes Graph-Material nicht, solange seine Pipeline im Hintergrund baut. Der Baker wartet über `IRenderer::PumpShaderBuilds()`; `HE_DUMP_SETTLEMS` ist die Krücke für die Witnesses.
2. Texturen mit fertiger Mip-Kette werden vom Packer **nicht noch einmal gekocht** (eine 2D-Textur wird nur aus einer einstufigen RGBA8-Quelle gebaut): der erste Export war 270 MB größer statt 69 MB. Jetzt einstufig.
3. Das Rechteck des Chunks als Konstante im Shader ergab 256 verschiedene Shader und 256 Pipeline-Builds (in den ersten Messläufen fehlten dadurch Chunks im Bild). Jetzt der Parameter `FarMap`; alle Chunks erzeugen **dieselbe** Shader-Quelle (Test).
4. `resolveEntitySlots` gibt für jedes Mesh, das nicht LOD0 ist, das Material der **Entity** zurück und überschrieb das Stufenmaterial: im echten Spiel 252 getauschte Objekte und ein bitgleicher Frame. Das Stufenmaterial wird nach diesem Schritt noch einmal gesetzt (Test in `test_culling`).
5. `tests/CMakeLists.txt` band `EditorInputMac.mm` nicht in `he_tests` ein (macOS-Link seit dem Pinch-Commit rot); behoben.

## Handoff

- **Topic / step:** 228, "Landscape-HLOD gebacken". Gebaut für Mac/Metal und die heutigen Chunks; Ziel später Thema 326 (Quadtree). Commits auf `release/0.8.0`: 7ac3fffd (BaseColor-Ansicht), ec9dbc04 (Rohausgabe + Baker), 156f74b9 (Assets, LOD-Material), 875b11f0 (Export), c128f03f (Extraktor-Fix, einstufige Texturen), danach "one shared shader".
- **Verified:** alles unter "Gemessen" in Abschnitt 7. Tests: ganze Suite 362 von 362 grün (danach nur `test_hlod_assets` und der gemeinsame Shader dazugekommen, beide grün).
- **Not verified:**
  * **Der Export-Dialog selbst.** Der Lauf ging über `HE_DUMP_HLODEXPORT` (dieselbe Funktion `bakeWorldLandscapes`) und den Export-Bench, nicht über den Knopf. Offen: ob `RenderSceneImage` mitten im ImGui-Frame des Dialogs sauber läuft, und wie der Export-Log aussieht. **Das bitte einmal von Hand ausführen** (Catania-Kopie, Export-Dialog, "Bake far landscape" an) und die Zeilen "Far landscape: ..." lesen.
  * **Andere Backends.** `rawOutput` gibt es nur auf Metal. GL/D3D/Vulkan: der Export meldet eine Notiz und lässt das echte Material. Das gebackene Material selbst ist ein gewöhnlicher Graph und sollte überall laufen; ungeprüft.
  * **Gemalte Layer.** Die Szenenkopie baut ihre Gewichtstextur selbst aus `layerWeights` (`TerrainSystem.cpp` um Zeile 669), also kommt Gemaltes wahrscheinlich an; **nicht per Bild geprüft** (Catania hat `layerWeightsB64`, der Vergleich im Spiel zeigt nur den Horizontstreifen).
  * Die Gruppen Forest, Grassland, Desert (ihre Array-Texturen fehlen in dieser Installation) und eine Landschaft ohne Auto-Landscape-Material (wird übersprungen, mit Notiz).
  * Wetter, Trails, Schnee in der Ferne: der Bake ist statisch (so entschieden, Abschnitt 2). Eine Landschaft, die das Spiel zur Laufzeit verformt, zeigt in der Ferne den Stand des Exports.
  * Windows, Linux; eine Landschaft mit Drehung oder Skalierung (wird übersprungen).
  * Im Editor (und im Play-in-Editor) taucht das gebackene Material nur auf, wenn die Assets als lose Dateien unter `Content/Baked/Landscape/` liegen (so habe ich gemessen); der Editor erzeugt sie nicht selbst.
  * Der Test `test_culling` für den Extraktor-Fix wurde nach der Reproduktion im echten Spiel geschrieben, nicht gegen den alten Code laufen gelassen.
- **Decisions taken:** Nutzer: automatisch beim Export, Albedo + Normal + Roughness, automatisches Downscaling. Von mir: gleiche Bilder für alle Fernstufen eines Chunks (die Mips tragen die gröberen), Aufnahme über unbeleuchtete Ansichtsgraphen, ein gemeinsamer Shader mit Parameter, Standardbudget 512 MB (nur ein Vorschlag).
- **Next steps, in dieser Reihenfolge:** 1. Export-Dialog von Hand (siehe oben). 2. Gemalte Layer prüfen (Gewichtstextur der Szenenkopie). 3. Windows: Rohausgabe für D3D11/D3D12/Vulkan (`EditorCameraOverride::rawOutput` in deren `RenderSceneImage`) und GL. 4. Für Thema 326: `bakeRect` je Quadtree-Kachel, Ids aus der Kachel, Planer mit mehreren Stufen, Inhalts-Hash-Cache (heute backt jeder Export neu; 3 s je 256 Chunks, das reicht noch).
