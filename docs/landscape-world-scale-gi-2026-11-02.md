# Landscape im Weltmaßstab: GI-Schatten auf der Höhen-Clipmap (Schritt 4)

Thema 326, Hive-Schritt 10 ("Schritt 4: GI, Schatten und Reflexionen auf der Höhen-Clipmap,
erneut, nachdem Schritt 2 verifiziert ist"). Vorherige Läufe dieses Schritts (1522/1830) wurden
von der Queen abgebrochen, bevor echte Arbeit begann — Schritt 2 (Quadtree-Renderer) war noch
nicht auf echter Hardware verifiziert. Das ist jetzt der Fall (Schritt 9/1598), darum erneut.

## 1. Befund zuerst: was die Clipmap überhaupt reichen kann

Vor jeder Codeänderung geprüft, weil der Plan GI-Schatten UND -Reflexionen in einem Atemzug
nennt, aber im Code zwei verschiedene Mechanismen sind:

* **GI-Sonnenschatten** laufen auf GL, Vulkan und D3D11 wahlweise über einen
  Höhenfeld-Pyramiden-March (`GiHeightfield.h`, `HE_GI_HEIGHTFIELD`), eine reine
  Performance-Optimierung: statt jedes Terrain-Chunk-Dreieck gegen den Sonnenstrahl zu prüfen,
  wird eine R32F-Mip-Pyramide gegen den Strahl marchiert (`EnsureGiHeightfield`,
  `kGiHeightfieldGLSL`/`kGiHeightfieldHLSL`). **Metal und D3D12 haben diesen Pfad nicht** (grep
  über `MetalRenderer.mm`/`D3D12Renderer.cpp`: null Treffer für `BuildGiHeightPyramid`) — ihre
  GI-Schatten laufen für JEDE Landschaft ausschließlich über die BVH/DXR-Trefferliste der
  tatsächlichen Mesh-Instanzen (`GiCasterFilter`, `m_giInstancesCpu`/DXR-BLAS).
* **GI-Reflexionen** laufen auf allen fünf Backends ausschließlich über die BVH/BLAS-Trefferliste
  (Normale + Albedo am Treffer, `GiLandscape.h`'s Paint-Tabelle für die Farbe). Die
  Höhenfeld-Pyramide wird dafür NIE gelesen (GL: `m_giInstancesCpu` statt
  `m_giShadowInstancesCpu`) — ein Höhenfeld kann keinen Closest-Hit mit Normale liefern, nur
  "ist dieser Strahl irgendwo blockiert". Das stand schon in der Planungswarnung
  (gpu-perf-reflections, Thema 326 Eröffnungspost) und wurde hier noch einmal am Code bestätigt.

Die Kachel-Landschaft (`LandscapeRuntime`) zeichnet seit Schritt 2 über den
`SetLandscapeProvider`-Haken, NICHT über `RenderWorld.objects` — sie hat also auf KEINEM Backend
eine BVH/BLAS-Instanz. Also:

| | heute (vor diesem Schritt) | nach diesem Schritt |
|---|---|---|
| GI-Sonnenschatten, GL/Vulkan/D3D11 | keine (Landschaft nicht in `heightfields`, nicht in der BVH) | **ja**, über die neue Clipmap |
| GI-Sonnenschatten, Metal/D3D12 | keine (keine BVH-Instanz) | unverändert keine — braucht BVH/BLAS-Geometrie der Patches, nicht dieses Schritts Umfang |
| GI-Reflexionen, alle fünf Backends | keine (keine BVH-Instanz) | unverändert keine, aus demselben Grund |

Das ist kein Rückschritt (Reflexionen und Metal/D3D12-Schatten waren vorher auch schon aus), und
es ist die ehrliche Grenze der Aufgabe: "Reflexionen auf der Höhen-Clipmap" ist mit einem
Höhenfeld allein nicht baubar, ohne die Patches selbst in eine BVH/BLAS zu geben — das wäre eine
neue, große Funktion (und berührt das `# Limits` des Schritts: "die Quadtree/Atlas-Struktur nicht
ändern", "den Ray-Marching-Algorithmus nicht ändern").

## 2. Was gebaut ist

**`HE::LandscapeGi`** (`src/HE_Scene/include/HorizonScene/LandscapeGi.h`,
`src/HE_Scene/src/LandscapeGi.cpp`), nach demselben Muster wie `LandscapeWater` (gleiches
Fenster-Konzept, gleiche Job-Infrastruktur):

* Fenster von `n x n` Kacheln um die Kamera (`n` so groß wie `kTerrainMaxResolution` (1025)
  erlaubt; bei der Standardgeometrie 256 Zellen/Kachel ist das `n = 4`, 2 km bei 2 m Raster —
  genau das, was ein einzelnes `TerrainComponent` heute an Auflösung bot).
* Hysterese wie bei `LandscapeWater`: das Fenster bleibt stehen, bis die Kamera die inneren
  `(n-2) x (n-2)` Kacheln verlässt (eine halbe Kilometer Spiel bei `n=4`) — kein Neubau bei jedem
  Kamera-Tick.
* Neubau läuft als Low-Priority-Job (`LandscapeWater`s Muster): `n²` Kacheln werden zu EINEM
  `(n·hCells+1)²`-Array geflacht (`landscapeRegionTerrain`, bereits aus Schritt 3c bekannt und
  getestet — Kachelnähte werden exakt geteilt, keine neue Naht-Logik nötig), das Ergebnis
  ersetzt die alte Clipmap als Ganzes auf dem Main-Thread.
* Fehlt eine Kachel des Fensters (Rand des Loads, frischer Start), wird NICHT neu gebaut — die
  alte Clipmap (falls es eine gibt) bleibt genau ein paar Frames "hinter den Kacheln zurück"
  statt GI-Schatten ganz abzuschalten. Kein Koarse-Welt-Fallback pro Texel: das hätte eine neue,
  ungetestete Stitching-Logik gebraucht, wo `landscapeRegionTerrain` (getestet, aus Schritt 3c)
  schon eine löst.
* Version wächst nur bei einem tatsächlich fertigen Neubau (nie 0, solange aktiv) — die Hysterese
  allein verhindert schon den "jeden Frame neu hochladen"-Fall, die Version ist der Zeuge dafür
  in den Tests.

**`HE::LandscapeGiHeightfield`** (im selben Header): die reinen Daten (`shared_ptr<const
vector<float>>`, `res`, `version`, `centreAbsolute`, `sizeX/sizeZ`) — bewusst OHNE
`HE::GiHeightfield` (HE_Rendering) als Typ, weil HE_Scene nicht gegen HE_Rendering linkt (dieselbe
Schicht-Regel wie `TerrainComponent::builtField`, das genauso nur rohe Felder trägt und erst
`RenderExtractor.cpp` daraus ein `GiHeightfield` baut).

**Verdrahtung** (kein Umweg über die ECS-Terrain-Schleife, die laut `LandscapeRuntime.h`
absichtlich nichts von der Kachel-Landschaft wissen soll):

* `HorizonWorld` bekommt einen nicht besitzenden Zeiger `landscapeRuntime()`/
  `setLandscapeRuntime()` (`HorizonWorld.h`, neben `origin()`) — nicht von `clear()` berührt,
  genau wie `origin()` von der Anwendung, nicht von der Szene gesetzt wird.
* `GameApplication::updateLandscape` und `EditorApplication`s Äquivalent setzen ihn jeden Frame
  (`m_world->setLandscapeRuntime(&m_landscape);`), direkt neben der schon bestehenden
  `SetLandscapeProvider`-Bindung.
* `RenderExtractor::extract` (neue freie Funktion `extractLandscapeGiHeightfield`, direkt nach
  `extractMeshes`) liest `world.landscapeRuntime()->giHeightfield()` und hängt — NUR wenn
  `out.heightfields` noch leer ist (eine Szene hat laut Doku entweder ein `TerrainComponent` oder
  eine Kachel-Landschaft, nie beide als GI-Caster) — einen `GiHeightfield`-Eintrag an. Dieselbe
  `heightfields.size() == 1`-Weiche, die GL/Vulkan/D3D11 schon für das alte `TerrainComponent`
  hatten, bleibt dadurch unverändert gültig: kein Backend-Code dieser drei Dateien wurde berührt.

## 3. Was NICHT gebaut ist (bewusst, siehe §1)

* GI-Reflexionen auf der Kachel-Landschaft, auf keinem Backend. Bräuchte BVH/BLAS-Geometrie der
  gezeichneten Patches — eine eigene, große Aufgabe, nicht dieser Schritt.
* GI-Schatten auf Metal und D3D12. Dieselbe Ursache (kein Höhenfeld-Marchpfad dort, nur
  BVH/DXR); ebenfalls nicht in diesem Schritt behoben.
* Koarse-Welt-Fallback innerhalb der Clipmap bei fehlenden Kacheln (siehe §2: bewusst die alte
  Clipmap behalten statt eine neue Stitching-Logik zu schreiben).

## 4. Verifiziert

* `he_tests --source-file=*test_landscape_gi.cpp` — 6 neue Testfälle, 54 Assertions, grün.
  Deckt: Hysterese (Fenster bleibt stehen in den inneren Kacheln), Version wächst nur bei
  echtem Neubau, Weltrand (Fenster verlässt die Welt nicht), fehlende Kachel (deferred, kein
  Absturz, keine falsche Aktivierung), kleinere Welt als das Fenster, `n` auf das Limit
  geklemmt (`kTerrainMaxResolution`), `LandscapeRuntime`-Verdrahtung (`giHeightfield()`,
  `reset()` räumt auf) und die `RenderExtractor`-Verdrahtung direkt (`out.heightfields` bekommt
  genau einen Eintrag, `worldToLocal` bildet das Fensterzentrum exakt auf lokal (0,0,0) ab —
  die Konvention, die der Shader mit seinem "+0.5" voraussetzt).
* `he_tests --source-file=*test_gi_*.cpp --source-file=*test_landscape_*.cpp
  --source-file=*test_terrain_tessellation.cpp` — 24 Testfälle, 100687 Assertions, grün, keine
  Regression.
* Build: `HorizonEditor`, `he_tests`, `landscape_gpu_check` auf NN-WS03 (MSVC, Release) — alle
  drei grün gebaut, mit `RenderExtractor.cpp`-Objektdateien nachweislich neu übersetzt
  (Zeitstempel-Kette: Quelländerung → `.obj` → `HorizonRendering.dll` → in jedes Ziel kopiert/
  gelinkt, alles nach der Änderung).
* `landscape_gpu_check --only <backend>` (je Backend ein eigener Prozess, wie Lesson 613
  verlangt), `HE_HIDDEN_WINDOW=1`: **GL OK, Vulkan OK.** D3D11 und D3D12 melden FAILED, aber an
  genau der Stelle, die schon vor diesem Schritt als offener Windows-Punkt bekannt war: dem
  `world uv, normal + mask`-Check (Normal-Map-Beleuchtung und AO bleiben auf D3D11/D3D12 flach,
  123.48 bei jeder Variante, während GL/Vulkan 57.75..148.67 je Richtung zeigen) —
  "D3D11/D3D12 still don't read Normal/Mask", bereits im Windows-Auftrag von Schritt 1520 als
  eigener, separater Punkt (1.2) geführt. Kein Treffer, der GI oder die Clipmap nennt; der Rest
  des Checks (Rampen-Patches, Kamerasprung, Hügel, Engine-Check Vorwärts/Deferred,
  100 % Boden-Treffer) ist auf allen vier Backends grün. Kein Regressionsbefund durch diesen
  Schritt.

## 5. Nicht verifiziert

* Kein Pixel-Vergleich mit `landscape_gpu_check` zwischen GI-an und GI-aus auf der Kachel-
  Landschaft (das Werkzeug kennt heute nur die Quadtree-Geometrie, keinen GI-Schalter) — eine
  Erweiterung dafür wäre ein eigener, nicht-trivialer Schritt in `landscape_gpu_check`s Engine-
  Check, nicht in diesem Schritt gebaut.
* Keine Bildprobe im Editor/Spiel mit einer echten Kachel-Landschaft + Sonnenlicht (bräuchte
  einen echten Pack oder eine größere prozedurale Testwelt im Editor, nicht nur die
  Unit-Test-Welten).
* D3D12/Vulkan-Validierungsfehler mit GI an: nicht neu geprüft (dieser Schritt ändert an D3D12
  nichts, an Vulkan nur den CPU-seitigen Pyramiden-Inhalt, nicht die Shader/Pipeline selbst).

## Handoff

- **Topic / step:** 326, Hive-Schritt 10 ("Schritt 4: GI, Schatten und Reflexionen auf der
  Höhen-Clipmap, erneut")
- **Branch / commit:** `claude/landscape-weltmassstab-100km` @ <siehe `git log -1`> (gepusht:
  siehe Ergebnis-Post)
- **Done:** GI-Sonnenschatten auf der 100-km-Kachel-Landschaft funktionieren jetzt auf GL,
  Vulkan und D3D11 (dieselben drei Backends, die den Höhenfeld-March-Pfad schon für das alte
  `TerrainComponent` hatten) — eine Clipmap aus den residenten Kacheln um die Kamera ersetzt
  das alte `builtField` einer einzelnen Landschaft.
- **Files touched:**
  - `src/HE_Scene/include/HorizonScene/LandscapeGi.h`, `src/HE_Scene/src/LandscapeGi.cpp` — neu,
    die Clipmap selbst (Fenster, Hysterese, Job, Version)
  - `src/HE_Scene/include/HorizonScene/LandscapeRuntime.h:+`, `src/LandscapeRuntime.cpp:+` —
    `m_gi`, `giHeightfield()`/`giStats()`, aufgerufen in `updateAt()`, geräumt in `reset()`
  - `src/HE_Scene/include/HorizonScene/HorizonWorld.h:+` — `landscapeRuntime()`/
    `setLandscapeRuntime()`, nicht von `clear()` berührt
  - `src/HE_Rendering/src/RenderExtractor.cpp:~604` — `extractLandscapeGiHeightfield` (neue
    freie Funktion, nach `extractMeshes`), Aufruf in `extract()`
  - `src/HE_Game/src/GameApplication.cpp:~1618`, `src/HE_Editor/EditorApplication.cpp:~4089` —
    je eine Zeile, setzt `landscapeRuntime()` jeden Frame
  - `src/HE_Scene/CMakeLists.txt`, `tests/CMakeLists.txt` — neue Quelldatei/Testdatei
    eingetragen
  - `tests/test_landscape_gi.cpp` — neu, 6 Testfälle
- **Verified:** siehe §4 oben.
- **Not verified:** siehe §5 oben.
- **Decisions taken:**
  - Clipmap-Quelle ist `landscapeRegionTerrain` (Kacheln, getestet, Nähte exakt), nicht ein
    Koarse-Welt-Fallback pro Texel — Begründung in §2/§3.
  - Reflexionen und Metal/D3D12-Schatten bleiben unverändert aus (§1) statt eine BVH/BLAS-
    Instanz für die Patches zu bauen — das wäre ein eigener, großer Schritt und würde die
    Quadtree/Atlas-Struktur bzw. den Ray-Marching-Algorithmus berühren, beides laut `# Limits`
    dieses Schritts verboten.
  - Die `heightfields.size() == 1`-Weiche der drei Backends bleibt unverändert: die neue
    Clipmap wird nur angehängt, wenn noch kein `TerrainComponent`-Eintrag da ist.
- **Decisions suggested, not taken:** Ein Pixel-Check in `landscape_gpu_check` für GI an/aus auf
  der Kachel-Landschaft wäre der nächste harte Beleg für "Done when" — braucht einen GI-Schalter
  im Werkzeug, nicht nur in der Engine.
- **Pending:** GI-Reflexionen und Metal/D3D12-Schatten auf der Kachel-Landschaft (BVH/BLAS der
  Patches) — vermutlich ein eigenes Thema, nicht ein Nachtrag zu diesem Schritt.
- **Traps:** `LandscapeRuntime::begin`/`beginFromPack` verlangt `loadRadius >=
  LandscapeQuadtreeConfig::minLoadRadius()`, abhängig von der Geometrie — eine Testwelt mit sehr
  kleinen Testkacheln (`hCells=32` o.ä.) UND der Standard-`LandscapeRuntimeConfig` schlägt mit
  "loadRadius ... ist unter dem, was die feinen Stufen brauchen" fehl; für
  `LandscapeRuntime`-Integrationstests die reale Geometrie nehmen (wie
  `test_landscape_water.cpp`s eigener Runtime-Test es schon tut), kleine Testgeometrien nur für
  reine `LandscapeStore`/`LandscapeGi`-Tests ohne `LandscapeRuntime::begin`.
- **Next steps:**
  1. Einen GI-Schalter in `landscape_gpu_check`s Engine-Check einbauen (GI an/aus, Schattenmaske
     dumpen) und damit den Pixel-Beleg aus §5 nachreichen.
  2. Falls Reflexionen/Metal/D3D12-Schatten auf der Kachel-Landschaft gewollt sind: ein eigenes
     Thema für "Patches als BVH/BLAS-Instanzen", nicht an diesem Schritt weiterbauen.
  3. Schritt 5 (Entladen ganzer Regionen, HLOD) kann unabhängig von hier weiterlaufen.
