# Landscape-HLOD mit gebackenem Material (Entwurf für Thema 326)

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

## Handoff

- **Topic / step:** 228 (Wunsch des Nutzers), Ziel Thema 326 Schritt 2 und 5. Entwurf, kein Laufzeit-Code.
- **Done:** dieses Dokument; `HE::landscape::planHlodBake` (Header, 4 Testfälle, 74 Prüfungen grün); Hive-Einträge in 228 und 326.
- **Verified:** die Rechnung des Planers (per Hand nachgerechnet und im Test festgenagelt); das G-Buffer-Layout aus der Codegen-Kopfzeile in `MaterialGraph.cpp`; dass Deferred nur auf Metal und GL existiert (Memory/Plan); dass es heute keine LOD-Stufe mit eigenem Material gibt (`TerrainSystem.cpp`).
- **Measured (Abschnitt 3a):** Encode-Durchsatz und Pack-Aufruf je Kachelseite (ASTC/BC7/BC3), zstd-Ersparnis auf einem echten Top-down-Render des Auto-Landscape-Materials. **Abgeleitet:** die Render-Zeit je Kachel (aus dem Audit vom 21.10.).
- **Not verified:** ein echter Bake (es gibt den Pfad nicht); die Kompression eines unbeleuchteten, reinen Albedo-Bakes; die GPU-Zeit je Kachel am echten Aufruf; ob der Renderer-Teil B von Thema 326 schon einen Offscreen-Kachel-Pfad hat (ich habe nur die Dokumente und das Tile-Format gelesen); Windows und Linux.
- **Decisions taken:** Nutzer: erst Quadtree, automatisch beim Export, Albedo + Normal + Roughness. Von mir: Aufnahme des G-Buffers statt neuer Shader; Inhalts-Hash-Cache; Downscaling-Regeln wie in Abschnitt 3.
- **Next steps:** 326-Lead liest Abschnitt 6; Zeit/Raster-Messung an einer Kachel; Bake-Funktion im Renderer (Metal und GL zuerst); Windows: MRT-Pfad für D3D/Vulkan.
