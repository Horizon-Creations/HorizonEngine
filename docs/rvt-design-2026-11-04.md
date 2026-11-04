# Runtime Virtual Textures für Landscape (Thema 333)

Ziel (vom Menschen): wie in Unreal. Das Landscape-Material zeichnet **einmal** top-down in eine Runtime
Virtual Texture, die Landschaft liest danach nur noch. Segmentierbar je Landscape-Tile. Der eigentliche
Material-Shader läuft nur, wenn sich etwas ändert.

Dieses Doc ist Schritt 1 (Entwurf + CPU-Kern, kein GPU-Code). Schritte: Hive-Thema 333, `hive_steps`.

## 1. Bestand, auf dem das aufsetzt

| Was | Wo | Nutzen für RVT |
|---|---|---|
| Drei unlit Ausgabegraphen des Auto-Landscape (`BaseColor`, `Normal` = Weltnormale·0,5+0,5, `Surface` = R AO / G Roughness) | `AutoLandscapeView`, `src/HE_Core/include/MaterialGraph/AutoLandscapeMaterial.h:289` | Das ist schon der Inhalt einer Page. Der Producer braucht keine neue Material-Logik. |
| `HlodBaker` rendert damit top-down ortho, wartet mit `IRenderer::PumpShaderBuilds` auf Pipelines | `src/HE_Scene/include/HorizonScene/LandscapeHlodBaker.h` (Thema 228) | Vorbild für Kamera und Warten. Aber ein ganzer Szenen-Pfad (`RenderSceneImage`): für RVT zu schwer, Schritt 2 baut einen schlanken Offscreen-Pass. |
| Fallen aus 228: Rechteck als Shader-Konstante = ein Shader je Chunk; Graph-Material zeigt sich nicht, solange die Pipeline baut | `docs/landscape-hlod-baked-material-2026-11-02.md` §7 | Page-Rechteck ist ein **Parameter**, alle Pages teilen **einen** Shader. Erste Pages erst, wenn die Pipeline fertig ist. |
| Tile-Landschaft (Quadtree, `LandscapeStore`, `kLandscapeFS`) | Thema 326, PR 214, **nicht** auf `release/0.8.0` | Consumer der Schritte 3/5. Vorher per `hive_post` an 326 abstimmen. |

## 2. Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Page-Größe | 128 Nutztexel + 4 Rand je Seite = 136² physisch | Rand hält Bilinear/Aniso auf der Page; 136 ist durch 4 teilbar (Block-Kompression später). |
| Feinste Auflösung | 0,25 m/Texel (4 Texel/m) → mip-0-Page = 32 m | Entspricht dem, was `planHlodBake` für die Nähe liefert (0,49 m/Texel gedeckelt bei 256). Je Volume einstellbar (`finestMetersPerTexel`). |
| Layer | **Revidiert in Schritt 2:** drei RGBA8-Arrays, eins je Ansicht: 0 BaseColor (sRGB), 1 Normal (Weltnormale·0,5+0,5), 2 Surface (R AO, G Roughness) | Die drei Ansichtsgraphen aus 228 haben je **einen** Ausgang; ein Draw in zwei gepackte Ziele bräuchte MRT im Material-Codegenerator, und eine Schreibmaske kann AO/Roughness nicht von RG nach BA schieben. Also drei Draws je Page. Das ursprüngliche Packing (BaseColor+A / Normal.xy+Rough+AO) bleibt eine spätere Optimierung (Schritt 6, mit Kompression). |
| Pool-Größe | 136²·12 B = 222 KB je Page unkomprimiert (3 Layer); 256 MB ≈ 1180 Pages. Kompression (BC7/BC3 auf der GPU) erst in Schritt 6 | Erst Korrektheit, dann Speicher. Schalter `poolSlots` ist das Budget. |
| Mip-Kette | Seitenzahl wird auf 2er-Potenz aufgerundet, Mips bis **eine** Page für das ganze Volume | Page-Table-Mips halbieren sauber; Fallback-Kette endet immer. |
| Welche Pages werden gebraucht | Mip m deckt das Quadrat mit Halbkante `nearRadius·2^m` um die Kamera (Kanten inklusive). Alle Quadrate sind ineinander geschachtelt | Jeder Mip braucht ungefähr gleich viele Pages (bei 64 m / 32 m-Pages: 25), also ~125 Pages je Volume in Sicht. Die Fallback-Kette hat keine Lücken, weil jede Page einen Vorfahren im größeren Quadrat hat. |
| Reihenfolge beim Füllen | Zuerst Pages ohne irgendeinen Vorfahren (grobe zuerst), dann der Rest nach Abstand zur Kamera; Budget `maxRendersPerFrame` | Die Sicht ist sofort grob abgedeckt, Detail rückt von der Kamera nach außen nach. |
| Eviction | LRU, nie eine Page, die die aktuelle Ansicht braucht, nie eine gepinnte (die `pinnedMips` gröbsten Mips) | Zu kleiner Pool = **starved** (Zähler), kein Flackern durch Verdrängen des Sichtbaren. |
| Änderungen | `invalidateRect(Weltrechteck)` markiert genau die Pages, die es (mit Rand) berühren, über alle Mips; sie bleiben sichtbar (alter Inhalt) bis sie neu gemalt sind. `invalidateVolume` für Parameter-/Switch-/Material-Wechsel | Ein Pinselstrich kostet 1 Page je Mip, nicht das Volume. |
| Stillstand | Eine Frame ohne Änderung und Kamerabewegung gibt eine **leere** Liste zurück und bumpt `tableVersion` nicht | Das ist die Zusage „Shader läuft nur bei Änderung“; getestet. |
| Segmentierung | Ein Volume je Tile (bzw. Region), alle teilen **einen** Pool. `removeVolume`/`dropVolumePages` gibt die Slots sofort zurück | Globales Budget statt Budget je Tile. Id wird wiederverwendet. |
| Wetter, Schnee, Trails | **Statisch** im RVT (nicht einbacken); sie bleiben ein billiger Live-Overlay im Consumer, falls nötig (Entscheidung final in Schritt 3). Nicht umgesetzt, nur vorgeschlagen | Sonst wäre das ganze Volume bei jeder Wetteränderung dirty. |
| Verhältnis zum Export-Bake (228) | RVT ersetzt ihn zur Laufzeit. `HlodBaker`/`LODLevel::materialId` bleiben als Notnagel für Backends ohne RVT und für den Export | Kein Rückbau in diesem Thema. |
| Backend ohne RVT (`supportsRvt == false`, Software) | läuft wie heute: das Material direkt | Wie `supportsRawCapture` aus 228. |

## 3. API (Schritt 1, `HE_Scene`, `HorizonScene/RuntimeVirtualTexture.h`, `namespace HE::rvt`)

* `VolumeLayout`: Geometrie. `pageRect`, `pageAt`, `mipForMetersPerPixel`, `pagesPerSide(mip)`, `mipCount()`.
* `RvtManager(poolSlots, maxRendersPerFrame, pinnedMips)`: `addVolume`, `removeVolume`, `beginFrame()`, `update(volume, ViewDesc) -> vector<PageRender>`,
  `invalidateRect/Volume`, `resolve`, `pageTable(volume, mip)` (Fallback eingetragen; Eintrag = `slot | effektiverMip << 24`, `0xFFFFFFFF` = leer),
  `tableVersion`, `stats()`.
* `PageRender` = was der Producer **diese Frame** malen muss: Schlüssel, Pool-Slot, Weltrechteck mit Rand, `repaint`.
  `update()` nimmt an, dass der Producer die Liste noch in derselben Frame malt.

## 4. Grenzen und Risiken (ehrlich)

* **Gepinnte Mips bei vielen Tiles:** 2 gepinnte Mips = 5 Pages je Volume. Bei einem Volume je 512 m-Tile und ~800 geladenen Tiles (8 km Radius, Thema 326) wären das 4000 Pages Dauerlast.
  Für die Tile-Landschaft muss `pinnedMips` 0 bzw. 1 sein, oder die Ferne kommt aus einem gröberen Regions-Volume (Schritt 5). Offen.
* **Mehrere Volumes in einer Frame:** `update()` je Volume nacheinander. Ein früheres Volume darf Pages eines späteren verdrängen (sie sind älter), bis dieses `update()` läuft und sie neu anfordert. Bei ausreichend großem Pool kein Effekt; bei zu kleinem Thrash. Schritt 5 soll die Ansichten aller Volumes zuerst „berühren“ und dann verteilen.
* **Hysterese fehlt:** Pages außerhalb der Ansicht bleiben, bis der Platz gebraucht wird (LRU). Kein Zeitlimit.
* **Kein GPU-Code:** Pool, Page-Table-Textur, Producer-Pass, Consumer-Shader, Kompression, Debug-Ansicht kommen in den Schritten 2-7.
* `nearRadius` ist hier ein fester Wert in Metern. Die Ableitung aus Auflösung/FOV (wie `planHlodBake`) gehört zum Consumer (Schritt 3), der weiß, welche Texeldichte der Schirm will.

## 5. Verifikation dieses Schritts

* `tests/test_rvt_core.cpp` (9 Fälle, 1002 Prüfungen), eingetragen in `tests/CMakeLists.txt`, `RuntimeVirtualTexture.cpp` in `src/HE_Scene/CMakeLists.txt`.
* Gebaut und gelaufen **eigenständig** (MSVC `cl`, doctest-Main, nur `RuntimeVirtualTexture.cpp` + Test): 9/9 grün.
* Negativkontrolle: ohne das „Page ist in Sicht, also anfassen“ in `update()` schlägt der Eviction-Fall fehl (5 Prüfungen rot). Der erste Anlauf dieses Tests hat die Mutation **nicht** gefangen und wurde deshalb verschärft (geteilte Pages beider Ansichten, `evictions == 15`).

## Schritt 2: Producer-Vertrag und D3D11 (2026-11-04)

Entscheidung: **kein neuer Render-Pfad.** Die D3D11-Zeichenschleife für Graph-Material (Per-Objekt-Konstanten, Gewichtstextur, Overrides, Switches) steckt als Lambda mitten in `DrawScene`;
sie nachzubauen würde still falsche Gewichte oder fehlende Overrides erzeugen. Eine Page ist deshalb ein `RenderSceneImage` mit Rohausgabe (der Weg von `HlodBaker`), dessen fertiges Bild die GPU **ohne Readback** in die Pool-Slice kopiert.
Ein schlanker eigener Pass bleibt eine Optimierung, wenn die Messung (Schritt 8) ihn braucht.

| Baustein | Wo |
|---|---|
| `EditorCameraOverride::rvtSlot/rvtLayer`, `Capabilities::supportsRvt`, `IRenderer::CreateRvtPool / DestroyRvtPool / ReadbackRvtSlot` (Default: false/no-op, andere Backends unverändert) | `src/HE_Core/include/Renderer/IRenderer.h` |
| D3D11: drei `R8G8B8A8_TYPELESS`-Arrays (SRV sRGB für Layer 0), `CopySubresourceRegion` aus dem Viewport-Bild statt `CaptureViewport` | `D3D11Renderer.cpp` (`RenderSceneImage`, `CreateRvtPool`, `ReadbackRvtSlot`), `D3D11Renderer.h` |
| `HlodBaker::renderRvtPage(rect, px, slot)`: die drei Ansichten einer Page in die drei Layer | `LandscapeHlodBaker.h/.cpp` |
| `HE::rvt::paintPages(baker, vector<PageRender>, pagePx)` | `HE_Scene/RvtProducer.h/.cpp` |
| Zeuge `HE_DUMP_RVTTEST=1` (mit `HE_DUMP_AUTOLAND=1 HE_DUMP_HLODBAKE=128`), nur Editor, nicht in `he_tests` (CI hat keine GPU) | `EditorApplication.cpp`, hinter dem HLODMOSAIC-Block |

Gemessen (D3D11, RTX 4070, Debug-Build, Witness-Landschaft 256 m, 136-px-Pages):
* 9 Pages gefragt, 9 gemalt, **7,9 ms je Page** (Debug, drei volle Szenen-Frames je Page; Release und ein schlanker Pass sind offen).
* Slice gegen den Lesepfad (`bakeRect` desselben Rechtecks): **27 von 27 Slices byte-identisch**, größte Abweichung 0.
* Rand gegen Nachbar-Nutztexel (Spalten 128..135 von Page (x,y) = 0..7 von (x+1,y)): Albedo mittlere Abweichung 0,11-0,13 /255, max 2; Normal 0,000. Das prüft Texelraster und Y-Ausrichtung zwischen Pages.
* Stillstand: zweites `update()` mit gleicher Kamera: 0 Pages.
* Kontrolle, dass der Zeuge nicht leer ist: Mitte der Page 0 hell (`f5efeb`), schwarz sind genau die 2112 Randtexel (136² − 128²) außerhalb der Landschaft.
* **Analytischer Positionstest** (`HE_DUMP_RVTTEST`, Teil 4): die drei Ansichten werden durch ein unlit Material `fract(Weltposition / 8 m)` ersetzt (`HlodBaker::setViewMaterials`); jedes Texel von 4 Mip-0-Pages (2 x 2, also mit allen Rändern und Nähten) wird gegen den auf der CPU berechneten Wert am Texelmittelpunkt `rectMin + (i+0,5)·texelM` verglichen, Zeile 0 = min-Z. R trägt x, B trägt z (G wäre die Geländehöhe).
  * Ohne Fehlerinjektion, alle drei Layer: **68 644 Texel, 0 falsch (> 3/255), schlimmster Wert 1/255** (Layer 0 sRGB) bzw. 0 (Layer 1, 2); 5 340 Texel außerhalb der Landschaft ausgelassen.
  * Negativkontrolle `HE_DUMP_RVTTEST_FAULT` (macht die Erwartung absichtlich falsch): `half` (halbes Texel) Layer 1/2 **alle 68 906 Texel falsch, schlimmster 8/255**, Layer 0 64 452; `flipz` (Z gespiegelt) alle falsch, bis 127/255; `swapxz` 64 350 falsch, bis 127/255. Der Test sieht also schon ein halbes Texel.
  * Damit sind Achsen, Zeilenrichtung (Zeile 0 = min-Z), Texelmitte und Randplatzierung für D3D11 belegt (nicht mehr nur über den Vergleich mit Thema 228).
* Schwäche, die bleibt: nur D3D11, nur eine flache Witness-Landschaft (256 m), Debug-Build. Pages in der Ferne (Mip ≥ 1) und andere Texeldichten sind nicht positionsgeprüft.
* **Build-Falle:** `cmake/HorizonSDK.cmake` installiert das Engine-SDK nach `HE_ENGINE_SDK_DIR` (Cache, Standard `<DEPLOY_DIR>/Editor/EngineSDK`), unabhängig vom `DEPLOY_DIR`-Override. Wer `DEPLOY_DIR` umlenkt, muss `-DHE_ENGINE_SDK_DIR=<repo>/out/deploy-rvt/Editor/EngineSDK` mit umlenken; sonst schreibt `HorizonSDKRelease` die neuen Header in den SDK-Ordner des laufenden Editors und der `dev`-Lib-Satz zeigt MISMATCH (der Build bricht ab).

Nicht in diesem Schritt: Page-Table-Textur und ihr Upload (kommt mit dem Consumer in Schritt 3, braucht dessen Format), Hysterese, andere Backends (Schritt 6), Pool-Kompression.
## Schritt 2b: Page-Rendering beschleunigt (gemessen vorher/nachher, 2026-11-04)

Frage des Menschen: nicht das ganze Szenenbild je Page, sondern nur der eine Shader in einen eigenen Framebuffer. Erst gemessen, wo die Zeit hingeht (D3D11, RTX 4070, **Release**, Witness-Landschaft, 9 Pages x 20 Wiederholungen, drei Ansichten je Page, CPU-Zeit mit Timern im Renderer):

| je Ansicht (ms) | vorher | nachher |
|---|---|---|
| Ziel-/HDR-Satz anlegen und freigeben (`createViewportRT`) | 0,067 (31 %) | 0 (ein Page-Ziel, einmal mit dem Pool angelegt) |
| `extract` der Szene | 0,042 (19 %) | 0,035 |
| Szenen-Pass | 0,103 (47 %) | 0,014 |
| davon `resolveShaders` (Quelle kopieren + hashen, je Draw) | **0,087 (40 % der Ansicht)** | 0,000 |
| Schatten, Himmel, Post-Kette | ~0 | entfällt |
| **gesamt je Ansicht** | **0,219** | **0,053** |
| **je Page (3 Ansichten, ohne Timer gemessen)** | **0,597 ms** | **0,106 ms (5,6x)** |

Befund: Schatten, Himmel und Post waren **schon vorher fast gratis**, weil der Baker in einer Szenenkopie mit nur einer Landschaft rendert. Die Idee "nur der eine Shader" war dort im Kern erfüllt; teuer waren das Anlegen der Ziele je Aufruf und `resolveShaders`, das bei **jedem Draw** den ganzen Fragment-Quelltext des Auto-Landscape-Graphen kopiert und hasht. Letzteres trifft jeden Graph-Material-Draw auf D3D11, nicht nur RVT.

Geändert (alles in `D3D11Renderer.cpp`, hinter `RenderSceneImage` mit `camera.rvtSlot >= 0`):
* `RenderRvtPage`: eigenes Page-Ziel (RGBA8 typeless mit UNORM- und sRGB-Ansicht, plus Tiefe), `DrawScene` direkt hinein, ohne Viewport-/HDR-Satz und ohne Post-Kette; Ausgabe per `CopySubresourceRegion` in den Pool. Der allgemeine Pfad bleibt Rückfall und Referenz.
* Schlüssel-Cache je Material (`matKeyCache`): gültig **innerhalb eines `DrawScene`-Aufrufs** (ein Hash je Material und Frame statt je Draw) oder, nur im Page-Pass, dauerhaft, weil dessen Ansichtsmaterialien dem Baker gehören. Warum nicht einfach über Zeiger und Größe: der Material-Editor weist den neuen Quelltext in denselben String zu und ruft `InvalidateMaterial` nicht auf, ein gleich langer Edit bliebe sonst unsichtbar. Greift nur ohne G-Buffer-/Instanz-Draw (die müssten sonst aus dem leer gelassenen Quelltext bauen).
* `HlodBaker::renderRvtPage` meldet ein Ansichtsmaterial dem Renderer nur einmal (`InvalidateMaterial`), nicht je Page (das hätte jedes Mal Cache und Texturen verworfen).

Geprüft nach der Änderung (Release): Positionstest unverändert **0 falsch, schlimmster Wert 1 / 0 / 0 /255**; die Fehlerinjektionen (`half`, `flipz`, `swapxz`) schlagen weiter an (8 / 120 / 127 /255); Rand gegen Nachbar wie vorher (0,11 /255, max 2); zweites `update()` 0 Pages; das normale Frame-Bild des Editors (`HE_DUMP_PATH`) **byte-identisch** zum Stand davor (belegt, dass der Cache das normale Zeichnen nicht ändert). Debug-`he_tests` (RVT, HLOD, Culling, Render): 144 Fälle grün.
Neu: die Pool-Slices sind jetzt **nicht** mehr byte-gleich zum alten Lesepfad (0 von 27, größte Abweichung 1/255). Der alte Pfad ging über ein RGBA16F-HDR-Bild und den Tonemap; der neue schreibt das Material direkt in 8 Bit und ist laut Positionstest auf den linearen Layern **exakter** als der alte. Der Positionstest ist damit das maßgebliche Orakel, nicht mehr der Vergleich mit dem Lesepfad.

Nicht gemessen / Grenzen:
* Der Gewinn im echten Spiel dürfte **größer** sein: dort lief der alte Pfad mit einem Live-Viewport und legte nach jedem Aufruf die HDR-Ziele in voller Größe neu an; die Witness hat keinen Live-Viewport. Nicht gemessen.
* Die Witness-Szene hat keine Lichter, daher keine Schatten; in einer Szenenkopie mit Lichtern käme der Schattenpass dazu (er ließe sich im Page-Pass abschalten, ist es noch nicht).
* Der Schlüssel-Cache im normalen Frame ist (bis auf Bildgleichheit) nicht auf Frame-Zeit gemessen.
* Übrig bleibt `extract` mit 66 % der Ansicht (0,035 ms): die drei Ansichten einer Page extrahieren dieselbe Szene dreimal. Nächster Hebel: einmal je Page extrahieren und nur das Material tauschen, oder mehrere Pages je Pass.
* Nur D3D11. Debug-Zahlen (7,9 ms je Page) waren irreführend: Release ist 13x schneller, und die gemessene Verteilung stammt aus Release.
* Build-Falle (zusätzlich zu `HE_ENGINE_SDK_DIR`): ein zweiter Baum mit `DEPLOY_DIR`-Override scheitert an `HorizonSDKRelease` (Prüfung des `dev`-Lib-Satzes: MISMATCH), sobald SDK-Header sich ändern und der Editor noch nicht gelinkt hat. Mit `-DHE_SDK_RELEASE_SET=OFF` im Release-Baum umgangen.

## Schritt 2c: eine Szenen-Extraktion je Page statt drei (2026-11-04)

Nach 2b war `extract` mit 66 % der größte Rest: die drei Layer-Ansichten einer Page extrahierten dieselbe Szene dreimal, weil der Baker das Ansichtsmaterial in die Komponenten der Landschaft schrieb.

Geändert:
* `EditorCameraOverride::rvtMaterial` (neu, `IRenderer.h`): mit `rvtSlot >= 0` zeichnet der Renderer jedes Objekt mit diesem Material. `HlodBaker::renderView` schreibt für Pages **nichts** mehr in die Welt (die Lesepfad-Ansichten tun es weiter).
* D3D11: das Material wird nach dem Extrahieren auf die Objekte gesetzt (`DrawScene`, hinter `extractUI`); `RenderRvtPage` öffnet bei Layer 0 den Frame-Cache des Extraktors (`beginFrame`) und schließt ihn bei Layer 2 (`endFrame`). Layer 1 und 2 finden die Szene dort, wo Layer 0 sie ließ. Der Vertrag des Extraktors (Welt ändert sich nicht, Kamera gleich) hält, weil die Layer sich nur in `rvtMaterial` unterscheiden, das erst nach dem Extrahieren angewandt wird.
* Sicherung gegen ein offen gebliebenes `beginFrame` (ein Layer scheitert): der erste normale `DrawScene` ruft `endFrame()`; sonst könnte ein normaler Frame eine fremde Szene wiederverwenden (der Extraktor erlaubt Reuse nur zwischen `beginFrame` und `endFrame`, D3D11 öffnet ihn sonst nie).
* Nebenwirkung, gewollt: ein Stellvertretermaterial für ferne LOD-Stufen (das gebackene Fernmaterial aus Thema 228) kann nicht mehr in eine Page geraten; sie zeigt immer das Ansichtsmaterial.

Gemessen (D3D11, RTX 4070, Release, ms je Page = drei Ansichten, 10 bis 20 Wiederholungen, Texel 0,25 m; "gesamt" enthält das Warten auf die GPU):

| Landschaft (Chunks) | Pages im Lauf | vor 2c gesamt | nach 2c gesamt | nach 2c nur CPU |
|---|---|---|---|---|
| 128 m (4) | 9 | 0,139 | **0,052** | 0,029 |
| 512 m (64) | 17 | 0,314 | **0,095** | 0,068 |
| 1024 m (256) | 21 | 0,436 | **0,211** | 0,191 |

Gegenüber dem allerersten Stand (0,597 ms bei 128 m) ist die Page jetzt rund **11x** schneller. Bei der großen Landschaft bleibt die CPU der Engpass (0,19 ms): sie wächst mit der Chunk-Zahl, weil jede Page die ganze Szenenkopie extrahiert, sortiert und beschneidet, obwohl sie nur einen Ausschnitt sieht. Nächster Hebel dafür: den Baker die Chunks der Szenenkopie auf das Page-Rechteck beschränken lassen (nur die berührten Chunks sichtbar), oder grobe Mips aus gröberen LOD-Chunks zeichnen. Nicht gebaut.

Geprüft: Positionstest jetzt auch bei **512 m und 1024 m** (vorher lief er dort leer, weil der Test den Mip-0-Bereich nicht ins Budget bekam; Fehler im Zeugen, behoben: 73 984 Texel, 0 falsch, schlimmster Wert 1 / 0 / 0 /255). 128 m: 68 644 Texel, 0 falsch. Fehlerinjektion `half` / `flipz` / `swapxz` schlägt weiter an (8 / 120 / 127 /255). Rand gegen Nachbar unverändert, zweites `update()` 0 Pages, das normale Editor-Bild byte-identisch zum Stand vor der RVT-Arbeit. Neue Zeugen-Schalter: `HE_DUMP_AUTOLANDSIZE=<m>` (128 bis 2048) und `HE_DUMP_RVTTEST_TEXEL=<m>`.

## Schritt 2d: Chunks der Szenenkopie auf das Page-Rechteck begrenzen: gemessen, verworfen (2026-11-04)

Annahme nach 2c: bei großen Landschaften wächst die CPU-Zeit mit der Chunk-Zahl, weil jede Page die ganze Szenenkopie extrahiert, sortiert und beschneidet. Versuch: der Baker setzt vor Layer 0 `MeshComponent::visible` nur für die Chunks, deren Rechteck das Page-Rechteck berührt (Rand 0,5 m), der Lesepfad `bakeRect` schaltet vorher alle wieder ein.

Messung (D3D11, RTX 4070, Release, 1024 m / 256 Chunks, Texel 0,25 m, 30 Wiederholungen, je dreimal im Wechsel; `DrawScene`-Phasen je Ansicht in ms, Mittel über die drei Ansichten einer Page, von denen zwei die Extraktion wiederverwenden):

| | extract | prep (Material setzen, Sortieren, Culling bis zum Graphen) | graph (Pässe) | Ansicht gesamt | Page gesamt (mit GPU-Warten) |
|---|---|---|---|---|---|
| alle Chunks sichtbar | 0,007 | 0,012-0,014 | 0,049-0,051 | 0,069-0,072 | 0,208-0,213 |
| nur berührte Chunks | 0,003-0,004 | 0,005 | **0,060-0,061** | 0,068-0,070 | 0,210-0,218 |

Ergebnis: extract und prep sinken zusammen um rund 0,011 ms je Ansicht, der Graph-Teil steigt um rund 0,010 ms. **Netto null.** Die Annahme war falsch: die Extraktion ist schon parallel (`parallel_for`), versteckte Chunks fallen früh heraus, und 256 Chunks kosten sie nur ~0,03 ms für den vollen Lauf. Dass der Graph-Teil mit weniger sichtbaren Chunks teurer wird, habe ich nicht erklärt (Verdacht: etwas im Pass hängt an der Änderung der Objektmenge von Page zu Page; nicht untersucht).

Konsequenz: nicht eingebaut (Patch nur im Scratchpad, nicht im Repo). Der Rest einer Page auf großen Landschaften (~0,07 ms je Ansicht) liegt im Graph-Teil (Pässe, Frame-Konstanten, Draws), nicht in der Szenenarbeit. Weiter beschleunigen würde nur noch ein Profil dieses Teils; bei 8 Pages je Frame sind es ~1,7 ms CPU, für Schritt 3 reicht das.

Messrauschen: derselbe Aufbau gab in zwei Läufen 0,211 und 0,273 ms je Page (Takt, GPU-Auslastung). Unterschiede unter etwa 25 % sind nur mit Wiederholung im Wechsel belastbar, wie oben.

## Schritt 3: der Consumer, die Landschaft liest Page Table + Pool (2026-11-04)

Entscheidung (nach Rücksprache über die Beschränkungen): der Consumer ist ein **Graph-Knoten** `RvtSample` plus **eine** gemeinsame Lookup-Funktion, nicht ein eigener Shader. So behält das Material die ganze Beleuchtung (Schatten, GI, Fog, Deferred), und `kLandscapeFS` (Thema 326) kann denselben Funktionstext später einbinden. Die Funktion steht in `src/HE_Core/include/MaterialGraph/RvtLookup.h`.

| Teil | Entscheidung |
|---|---|
| Lesen | **nur `texelFetch`**: das D3D11-Sampler-Budget ist voll (`kHlslMaterialPins`), also kein Sampler, bilinear von Hand (4 Taps je Layer). Die vier toten SamplerStates, die SPIRV-Cross trotzdem deklariert, liegen auf s0 neben dem toten `heAO`, die Form, die FXC akzeptiert. |
| Page Table | eine R32_UINT-Textur mit Mip-Kette, Mip m = (Seite >> m)² Einträge, `slot | effektiverMip << 24`, `0xFFFFFFFF` = nichts; Upload `IRenderer::SetRvtPageTable` bei Änderung von `tableVersion` |
| Bindungen | 40 Tabelle, 41 Albedo (sRGB-Ansicht, `Load` liefert linear), 42 Normal, 43 Surface; D3D11 t40..t43 (frei, geprüft), nur für Draws, deren Fragment `heRvtTable` liest, danach wieder ab |
| Volumenparameter | zwei vec4 als Eingänge des Knotens (A = Ursprung, Ausdehnung, Meter/Texel; B = Seitengröße, Rand, Seiten, Mips), normalerweise `Param Vector4` |
| Mip-Wahl | aus `dFdx/dFdy` der Weltposition (ein Texel je Pixel); die Seitenzelle wird beim **effektiven** Mip der gefundenen Seite neu berechnet (Fallback auf eine gröbere Seite) |
| Sichtbarkeit | Knoten nicht in der Editor-Palette; Vulkan-Layout, Vulkan-G-Buffer-Tails und D3D12-Root-Signatur haben keine Zeilen für 40..43: die Abdeckungstests für alle Knoten zäunen ihn ein und halten ihn auf genau diese vier Bindungen |

Geprüft (Debug `he_tests`, Release-Editor auf D3D11/RTX 4070):
* `test_rvt_shader`: Codegen-Text, t40..t43, nur `Load`, **echter FXC ps_5_0** kompiliert; die bestehenden Sweeps (Material-Graph, Landschaft, Engine-Materialien: 196 Fälle) grün.
* Page Table: Upload + Readback, jeder Eintrag gleich (21 Einträge bei 128 m, 1365 bei 1024 m).
* **Analytischer Consumer-Test** (`HE_DUMP_RVTTEST`, Teil 6): alle Pages mit einer Rampe (u, v, 0) bemalt, die Landschaft durch `RvtSample` gezeichnet, jeder Pixel gegen den CPU-Wert: alle Pages da, nur die oberste Page da (Fallback, effektives Mip), und ein feines Muster `fract(Welt/8 m)` (eine Rampe sieht einen Halb-Texel-Versatz nicht). Drei Ansichten (AO/Rauheit/gültig, sRGB-Basisfarbe, Normal). 128 m: 62 500 Pixel je Ansicht, **0 falsch, schlimmster Wert 2/255**; 512 m und 1024 m je rund 1,0 Mio. Pixel, ebenso. Fehlerinjektion: halbes Texel nur vom feinen Muster erkannt (9/255), Z gespiegelt und X/Z vertauscht von allen.
* **Lit gegen direktes Material** (`HE_DUMP_RVTTEST_LIT=1`, 128 m, 1280x720, Kamera 110 m darüber, Sonne/Himmel des Zeugen): mittlere Abweichung **1,55/255**, größte 116, 0,92 % der Pixel weichen um mehr als 16 ab (Bilder: `<dump>.rvt_lit_{direct,rvt,diffx4}.bmp`; die Muster liegen an denselben Stellen, die Abweichung sitzt im Feinkorn). Die 21 Pages der echten Landschaft in 1,8 ms bemalt.
* **Kosten** (15 abwechselnde Voll-Captures, Readback in beiden enthalten): direktes Material Median 3,60 ms (min 1,77), RVT Median 2,98 ms (min 1,14), also ~0,6 ms je Frame weniger. **Das ist eine grobe Zahl**: 720p auf einer 4070 mit einem 128-m-Ausschnitt, und je Capture kommen Zielanlage und Readback dazu. Eine echte Messung (GPU-Zeit des Szenen-Pass, große Landschaft, 1440p) steht aus.

Was der Consumer gegenüber dem direkten Auto-Landscape **verliert** (jedes ist eine Eigenschaft der Pages, nicht des Knotens):
* Displacement-Pin / Tessellations-Relief (die Seiten tragen keine Höhe; die Geometrie bleibt, das Relief der Textur nicht).
* Wetter (Nässe, Schnee-Bedeckung) und alles, was sich zur Laufzeit ändert, ist auf dem Stand der letzten Bemalung eingefroren (Entscheidung aus Schritt 1: statisch; Schritt 4 entscheidet über Dirty-Gründe).
* Normalen-Detail unterhalb der Seitenauflösung (0,25 m je Texel am Rand der Sicht), Metallic 0, Foliage-Metadaten (`LandscapeFoliage` ist ein Knoten ohne Ausgang im direkten Material: der Consumer **ersetzt das gespeicherte Material nie**, er ist ein Laufzeit-Stellvertreter).

Offen / nicht gebaut (bewusst, Schritt 4 ff.):
* **Wer ihn einsetzt.** Der Zeuge schreibt das Consumer-Material in die Komponenten der Live-Landschaft. Das darf nie im Spiel passieren (`SceneSerializer` würde es speichern): der Tausch gehört in Renderer/Extraktor, neben `rvtMaterial` und `LODLevel::materialId`. **Vorschlag, nicht entschieden:** der Consumer gilt für Chunks im aufgelösten Bereich des Volumes; ferne LOD-Stufen mit gebackenem Fernmaterial (Thema 228) behalten es.
* **Der Treiber je Frame**: `RvtManager::update` → `paintPages` → `SetRvtPageTable` bei neuer `tableVersion`; der Manager fragt nach Abstandsringen, der Shader wählt das Mip nach Ableitungen: das kostet höchstens Unschärfe oder überflüssige Pages, nicht Richtigkeit.
* Ein **globaler** Tabelle/Pool (t40..t43): mehrere Volumes (Segmentierung, Schritt 5) brauchen eine Tabelle je Objekt, wie die Gewichtstextur je Chunk.
* Kein trilineares Filtern zwischen Mips (die Mip-Grenze springt sichtbar), Tabelleneintrag `0xFFFFFFFF` zeigt neutrales Grau, `Valid` ist am Knoten abrufbar.
* Vulkan, D3D12, OpenGL, Metal: Zeilen für 40..43, Pool, Pass (Schritt 6). Der Tile-Quadtree-Shader (Thema 326): Lookup-Funktion einbinden.

## Handoff

- **Topic / step:** 333, Schritte 1 bis 3 von 8 gebaut (3 mit vier Unterschritten 3a-3d); Schritt 4 folgt.
- **Branch / commit:** `claude/runtime-virtual-textures`, gepusht (Stand vor 3b: 66fdc301; 3a 07fda632, 3b ae5e01ec, 3c 3561b01f, 3d siehe `git log`). Der Branch enthält auch lokale Commits der zweiten Session und zwei unveröffentlichte von `release/0.8.0` (1d57f608, db9afd77).
- **Done:** CPU-Kern (S1), Renderer-Vertrag und D3D11-Page-Pass 11x schneller (S2), Page Table, Knoten, D3D11-Bindung, Consumer-Test und Lit-Vergleich (S3).
- **Files:** `src/HE_Scene/{include/HorizonScene,src}/RuntimeVirtualTexture.*`, `RvtProducer.*`, `LandscapeHlodBaker.*` (renderRvtPage, renderMaterialImage, setViewMaterials); `src/HE_Core/include/MaterialGraph/RvtLookup.h`, `MaterialGraph.{h,cpp}` (RvtSample); `src/HE_Core/include/Renderer/IRenderer.h` (rvtSlot/rvtLayer/rvtMaterial, Pool, SetRvtPageTable); `src/HE_Rendering/src/Backends/D3D11/D3D11Renderer.cpp` (RenderRvtPage, matKeyCache, t40..t43); `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp` (Pins); `src/HE_Editor/EditorApplication.cpp` (Zeuge `HE_DUMP_RVTTEST` mit Teilen 1-7); `tests/test_rvt_core.cpp`, `tests/test_rvt_shader.cpp`, Zäune in `tests/test_material_graph.cpp`.
- **Zeugen-Schalter:** `HE_DUMP_AUTOLAND=1 HE_DUMP_HLODBAKE=128 HE_DUMP_RVTTEST=1` plus `HE_DUMP_AUTOLANDSIZE`, `HE_DUMP_RVTTEST_TEXEL`, `_PERF=<reps>`, `_FAULT=half|flipz|swapxz`, `_LIT=1` (`_LIT_REPS`). Immer `HE_HIDDEN_WINDOW=1`, eigenes `APPDATA`, `HE_DUMP_RHI=D3D11`.
- **Not verified:** Linux/macOS/Windows-CI für 3a-3d (gepusht sind nur S1/S2: Linux grün, macOS rot durch fremden Code `GpuArrayCook.cpp:170`, Windows lief noch), Release-GPU-Zeit im großen Maßstab, alle anderen Backends.
- **Traps:** zweiter Build-Baum mit `-DDEPLOY_DIR` UND `-DHE_ENGINE_SDK_DIR` umlenken, Release-Baum zusätzlich `-DHE_SDK_RELEASE_SET=OFF`; `he_tests` mit **eigenem APPDATA** starten (sonst wird die echte Konfiguration überschrieben); vor einem Push auf fremde Commits im Branch achten (`git log origin/release/0.8.0..HEAD`); `tests/CMakeLists.txt` hatte einen nie committeten Scratch-Test eingetragen (66fdc301).
- **Next steps:** 1. Schritt 4: Treiber je Frame + Tausch des Consumer-Materials in Renderer/Extraktor, Invalidierung (Pinsel, Parameter, Streaming). 2. Echte GPU-Zeit-Messung des Szenen-Pass (große Landschaft, 1440p). 3. Schritt 5: Tabelle je Objekt (Segmentierung), Absprache mit Thema 326 über `kLandscapeFS`.
