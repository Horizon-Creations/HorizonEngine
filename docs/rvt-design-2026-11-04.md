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

## Schritt 4: der Treiber, Invalidierung, Materialtausch (2026-11-04)

`HE::landscape::RvtLandscape` (`HE_Scene/RvtLandscape.h/.cpp`) fährt **eine** Landschaft. Pro Frame, nach dem Terrain-Update des Ticks: Journal leeren, Scratch-Kopie nachziehen, die Pages unter der Änderung als veraltet markieren, vom Seitenmanager holen, was die Kamera braucht (neue und veraltete, im Budget), genau das malen, die Tabelle hochladen, wenn sie sich geändert hat, und dem Renderer sagen, dass er das Landschaftsmaterial durch das Consumer-Material ersetzt (`IRenderer::SetRvtLandscape`), sobald die erste Füllung fertig ist. Ohne Änderung kostet ein Frame nichts: keine Page, kein Upload.

| Teil | Entscheidung |
|---|---|
| Wer sieht die Änderungen | ein **Journal** auf `TerrainComponent` (`rvtJournal`, nicht serialisiert): Sculpt-Pinsel (`markRegionDirty`) und Mal-Pinsel (`paint`) tragen ihr terrain-lokales Rechteck ein, `TerrainSystem` trägt "alles" ein bei einem Vollaufbau, einem Rückfall auf ungemalt und einem Gewichts-Upload, den kein Strich angekündigt hat (Füllen, Laden, Undo). `TerrainSystem` löscht es **nicht**: Strich, Neuaufbau und Treiber sind drei Zeitpunkte, und `updateTerrains` wird auch direkt von den Pinsel-Werkzeugen gerufen. Der Treiber leert es. Test `test_rvt_journal`. |
| Scratch-Kopie | `HlodBaker::syncTerrain`: bei "alles" eine volle Kopie + Neuaufbau; bei einem Rechteck werden nur die Höhen kopiert und nur die Chunks darunter neu gebaut (die Gewichtstextur teilt sich die Kopie mit der echten Landschaft, Malen braucht dort nichts). Der Höhenbereich der Page-Kamera wächst mit. `paintBegin/End` schalten den Renderer nur für den Moment des Malens auf die Kopie. |
| Parameter/Schalter des Materials | lassen sich nicht in die Pages flicken (die Ansichtsgraphen werden daraus gebaut): ein Hash über Parameterblock, überschriebene Parameter und Schalter, je Frame, bei Änderung wird neu angehängt (neue Kopie, alle Pages). |
| Tausch im Renderer | nach dem Extrahieren: `terrainChunk && materialAssetId == Landschaftsmaterial` bekommt das Consumer-Material **und leeren `paramOverride`** (der Block ist im Layout des alten Materials und würde `RvtVolumeA/B` überschreiben). Die Welt wird nie angefasst, `SceneSerializer` speichert nichts davon. Ferne LOD-Stufen mit gebackenem Fernmaterial tragen schon eine andere Id und bleiben dabei. |
| Einschalten | nur `HE_RVT=1` in Editor und Spiel (`EditorApplication`/`GameApplication`, ein Aufruf nach `tickWorld`), bis die Einstellung aus Schritt 7 da ist. Standard aus. |

Geprüft (D3D11, RTX 4070, Release-Editor, Zeuge `HE_DUMP_AUTOLAND=1 HE_DUMP_RVTDRIVER=1`; so wie die Anwendung es tut: `updateTerrains`, dann `tick`):

| | 128 m (21 Pages) | 512 m (Ring um die Kamera) |
|---|---|---|
| erste Füllung | aktiv nach 3 Frames, 21 Pages (höchstens 8 je Frame) | 7 Frames, 53 Pages |
| 10 Leerlauf-Frames | **0 Pages, 0 Uploads** | **0 / 0** |
| Sculpt-Strich (Beule 8 m) | **7 Pages als veraltet markiert, unabhängig gezählt 7**, 7 neu gemalt, 0 bleiben veraltet, Journal geleert | **13 = 13** |
| Bild im Strich: eigenes Material gegen die Pages | veraltet **4,19**, neu gemalt **1,39** (Kontrolle klar schlechter) | gleiche Werte |
| Mal-Strich (Ebene 2, 10 m) | 6 markiert (unabhängig ~6), 6 neu gemalt | 9 = 9 |
| Bild im Strich | veraltet **8,50**, neu gemalt **2,95** | gleiche Werte |
| Kamera +30 m (Ring 40/48 m) | nicht prüfbar: alle 21 Pages sind schon da | **4 Pages gemalt** (Budget 8), 0 veraltet |
| Materialparameter (Schneegrenze) | neu angehängt: ja | ja, 53 Pages in 6 Frames |
| Renderpfad | vorwärts 1,51, aufgeschoben (deferred) 1,55, Ausgangswert 1,55 /255 gegen das eigene Material | |

(Mittelwerte des größten Kanalunterschieds in /255.) Der Zähler allein beweist keinen Scratch-Abgleich: eine veraltete Kopie besteht jeden Zähler und fällt nur am Bild durch, deshalb die Kontrolle "Pages vor dem Nachziehen".

Nicht geprüft / bekannte Grenzen:
* **Der echte Editor-Loop mit Landschaft: geprüft** (2026-11-04, Release, D3D11; ein eigener Editor mit `HE_RVT=1`, `HE_MCP=1`, `HE_SKIP_PROJECT_PREFLIGHT=1`, eigenem `APPDATA`; das Projekt ist ein `{}`-`.heproj` mit der Szene aus `HE_DUMP_AUTOLANDSAVE`, 256 m; Bedienung nur über die MCP-Brücke: `scene_open`, `terrain_sculpt`, `terrain_paint`, `scene_screenshot`). Der Treiber wird jeden Frame gerufen, hängt sich an, füllt in 6 Frames 46 Pages (8 je Frame) und zeichnet danach aus den Pages; 6000 stille Frames melden nur den Zählerstand. Ein `terrain_sculpt` (+12 m, Radius 14 m): **11 Pages veraltet, 8 + 3 neu gemalt, 0 bleiben**; ein `terrain_paint` (Ebene 2): **alle 46 als veraltet markiert und in 6 Frames neu gemalt**, weil die erste Malung einer ungemalten Landschaft die Gewichtskarte anlegt und TerrainSystem das als "alles" verbucht (einmalig; ein Rechteck gäbe es erst für den zweiten Strich). Die Bilder (1280x720, gleiche Kamera) gegen einen zweiten Lauf mit `HE_RVT=0` und denselben Strichen: mittlere Abweichung **0,71 bis 1,05 /255, 0,03 bis 0,06 % der Pixel über 16**; Beule und Malung liegen an denselben Stellen.
* **Fehler, den dieser Lauf gefunden hat (behoben):** beim Beenden stürzte der Editor ab (Zugriffsverletzung in `HorizonScene.dll`, zweimal, beim Abbau). Der Destruktor von `RvtLandscape` gibt Pool und Tausch an den Renderer zurück, der zu dem Zeitpunkt schon abgebaut war. Der Treiber wird jetzt in `OnShutdown` freigegeben (Editor und Spiel), bevor der Renderer geht; danach 3000 Frames, Strich, Beenden: keine Absturzmeldung. Der Zeuge im Dump-Modus (ein Frame, `detach()` mit lebendem Renderer) und der Lauf ohne Landschaft (nie angehängt, nie am Renderer) konnten das nicht zeigen.
* **Das Spiel** (`GameApplication`) ist übersetzt und hat dieselbe Freigabe, aber **nicht gelaufen**.
* Nachtrag: der Treiber sagt jetzt, **warum** er untätig ist (einmal je Änderung, und alle 600 Ticks als Lebenszeichen) und meldet jeden Frame mit Aktivität (Pages, veraltet, Tabelle); ein stiller Treiber ließ sich sonst nicht von einem kaputten unterscheiden (so fiel auf, dass ein Lauf ohne `HE_SKIP_PROJECT_PREFLIGHT=1` stundenlang Engine-Content in das Scratch-`APPDATA` lud und der Editor das Projekt nie öffnete).
* **Wetter eingefroren auf trocken.** Der Auto-Landscape-Graph liest `Weather` (Nässe, Schneebedeckung); die Scratch-Kopie hat keine Umgebung, die Pages sind trocken, auch in einer nassen Szene. Stehendes Wasser (`waterF`) kommt aus Höhe/Regel und den gemalten Gewichten, nicht aus dem Wasserfeld, ist also vom Journal gedeckt. Die Umgebung in die Kopie zu spiegeln und bei einer Schwelle alles zu invalidieren ist nicht gebaut.
* **Tile-Laden/-Entladen** (der vierte Auslöser aus der Planung) gehört zu `LandscapeStore` von PR 214 (Thema 326), das auf diesem Branch nicht existiert: Schritt 5.
* Nur **eine** Landschaft, eine Tabelle, ein Pool (`CreateRvtPool` ersetzt den Pool). Mehrere Landschaften: Schritt 5 (unten).
* Kein Hysterese-/Zeitbudget (nur Seitenzahl je Frame), das Material der veralteten Pages bleibt bis zum Neumalen sichtbar.
* Ein erneutes Anhängen registriert drei Ansichtsmaterialien und ein Consumer-Material und entfernt die alten nicht (es gibt keine Unregister-Funktion), wie schon beim Export-Bake.

## Schritt 5: Segmentierung, mehrere Landschaften in einem Pool (2026-11-04)

**Ehrlich vorweg:** die echten Tiles (`LandscapeStore`, `kLandscapeFS`, Thema 326 / PR 214) gibt es auf diesem Branch nicht. Segmentiert wird hier in **mehreren `TerrainComponent`-Landschaften** (je ein Volumen, je ein Consumer-Material), im Zeugen als 2 x 2 aneinanderliegende 128-m-Landschaften. Das ist ein Stellvertreter: alles unten gilt für "ein Volumen je Kachel", nicht für den Streaming-Pfad des echten Tile-Landschaftssystems. Dessen Anschluss ist: `RvtLookup.h` (`heRvtSample`) in `kLandscapeFS` einbinden und je Tile einen Volumen-Parametersatz geben.

| Teil | Entscheidung |
|---|---|
| Seitenmanager (5a) | `RvtManager::updateAll(std::vector<VolumeView>)`: Phase 1 berührt die gewünschten Pages **aller** Volumen (nichts Gewünschtes wird verdrängt), Phase 2 rankt global `(Gruppe, Abstand, Mip, Volumen)` mit **einem** Budget; `missing(volume)` je Volumen. `update()` ist `updateAll({eins})`. Tests: Fairness gegen sequentielles Update als Kontrolle, Pool 20 Pages zu klein stellt sich ein ohne Ping-Pong, ein Volumen entfernen gibt genau seine Slots frei. |
| Renderer (5b) | der **Pool** ist einmal da; **Tabellen je Consumer-Material** (`SetRvtPageTable(consumer, mips)`, `ReadbackRvtPageTable(consumer, ...)`, `ReleaseRvtConsumer`); der **Tausch je Terrain-Entity** (`SetRvtLandscape(terrainEntity, landscapeMat, consumerMat)`, `RenderObject::terrainEntity` vom Extraktor): zwei Landschaften mit demselben Material bekommen verschiedene Consumer. |
| Shader (5c) | Kante: eine Page am **Rand des Volumens** hat hinter sich nichts (ihr Rand wurde aus einer Szene gemalt, die nur dieses Volumen enthält: schwarz), der bilineare Filter bleibt dort in der Nutzfläche (`pmin/pmax` in `heRvtSample`); der Rand einer inneren Page enthält das Nachbarland und wird gelesen. Nähte zwischen zwei Volumen sind also Kantenklemmung auf beiden Seiten, kein Blenden. |
| Treiber (5d) | `RvtLandscape` hat ein **Segment je Landschaft** (`Segment{baker, volume, consumer, stateHash, filled, swapOn, ...}`); der Pool entsteht einmal (`ensurePool`) und stirbt mit der letzten Landschaft; Aktivierung nach Abstand mit Hysterese (`activationRadiusM`, x1,2 zum Abschalten), **höchstens ein Anhängen je Frame** (`maxAttachesPerFrame`), ein Abschalten gibt die Pages zurück, **nicht** die Materialien; eine zerstörte Landschaft gibt ihr Volumen frei und schaltet ihren Tausch ab; Füllung-Latch und Tausch je Segment; eine Warnung **je Episode**, wenn der Pool zu klein ist. Kennzahlen: `FrameStats` (+ `attaches`, `detaches`, `segments`, `activeSegments`), `materialsRegistered()`. |

Geprüft (D3D11, RTX 4070, Release-Editor, Zeuge `HE_DUMP_AUTOLAND=1 HE_DUMP_AUTOLANDSIZE=128 HE_DUMP_RVTTILES=1`; vier Landschaften mit **einem** stetigen Gelände, `activationRadiusM` 400, `nearRadiusM` 160, Kamera 225 m darüber):

| | Ergebnis |
|---|---|
| Füllung | **4 von 4** nach 11 Frames, **4 Anhängen, höchstens 1 je Frame**, 84 Pages in **einem** Pool von 512 (je Kachel 21), Volumen 0..3 |
| Bild, eigenes Material gegen Pages (1024 x 1024, 0,25 m je Pixel, Bereich ohne den äußeren Rand) | Mittel **3,34 /255** (Maximum 117); in den Kacheln **3,36**, in den **Nahtstreifen (±1 m an den Grenzen) 2,60** : die Naht ist nicht schlechter als das Innere. Das Maximum liegt im Detailrauschen der Textur (Abweichungsbild x4: gleichmäßig feine Punkte, keine Linien an den Grenzen). Grenze des Tests: ein Naht-Fehler von einem Pixel Breite (4 Pixel Streifen) würde den Streifenmittelwert um ~25 heben, ein halbes Texel nicht. |
| Strich innerhalb Kachel 0 | erreicht **1** Kachel, **6** Pages veraltet (unabhängig gezählt 6), 6 neu gemalt, 0 bleiben |
| Strich über die Grenze Kachel 0/1 | erreicht **2** Kacheln, **10** veraltet (unabhängig 10), 10 neu gemalt, 0 bleiben |
| Kachel 3 zerstört | 84 -> **63** Pages (erwartet 63: 84 - 21), 3 Segmente, 3 aus Pages gezeichnet, 449 freie Slots |
| Weg und zurück (Kamera 5000 m weg) | weg: **0 Pages, 0 Kacheln aus Pages** (das Material der Landschaft wird wieder direkt gezeichnet); zurück: 3 Kacheln nach 7 Frames; **Materialien 16 -> 16** (kein Zuwachs durch Reaktivierung) |
| Pool von 40 Pages für 3 Kacheln (63 gewünscht) | 40 belegt, **0 Verdrängungen**, danach in 30 ruhigen Frames **0 Pages gemalt, 0 Verdrängungen** (kein Ping-Pong); eine Warnung "pool is full" (je Episode, vorher jeden Frame) |

Dazu unverändert grün: `HE_DUMP_RVTTEST` (Verbraucher 0 falsche, Feinmuster 36 100 Pixel 0 falsche) und `HE_DUMP_RVTDRIVER` (Füllung 3 Frames/21 Pages, Leerlauf 0/0, Sculpt 7=7, Mal 6~6, aufgeschoben 1,51 gegen vorwärts 1,47 /255), und `he_tests` (Debug, eigenes `APPDATA`): 422 Testfälle, 554 210 Assertions, 0 Fehler (test_rvt*, hlod, material_graph, terrain, landscape, culling).

Nicht geprüft / bekannte Grenzen:
* **Die echten Tiles** (siehe oben), und **der echte Editor-Loop mit mehreren Landschaften** (MCP-Brücke): nur der Zeuge, der den Treiber so ruft wie die Anwendung (`updateTerrains`, dann `tick`), und der Einzel-Landschaft-Loop aus Schritt 4. Der Abbau mit mehreren Segmenten läuft im Zeugen über `detach()` bei lebendem Renderer, nicht über `OnShutdown`.
* **Ein zu kleiner Pool zeigt gar nichts aus Pages**: das Ranking arbeitet nach Abstand über alle Volumen, bei einer Kamera genau über dem Zusammentreffen (hier) füllt sich jede Kachel halb, keine wird fertig, alle bleiben auf dem direkten Material (gutmütig, aber der Speicher ist belegt). Mit einer Kamera, die einer Kachel näher ist, wäre die zuerst fertig; "fertig vor halb" ist nicht gebaut. Die Warnung nennt `poolSlots` und `activationRadiusM`.
* Die Tabelle je Consumer lädt die **ganze** Tabelle neu, wenn sich eine Seite ändert (kleine Mips, ein paar KB); kein Teil-Update.
* Das Spiel (`GameApplication`) ist übersetzt, nicht gelaufen. Nur D3D11. Das Wetter bleibt trocken (Schritt 4).

## Schritt 6: D3D12, Vulkan und OpenGL (2026-11-04)

Dieselbe Aufgabe in drei Backends, alle nach dem D3D11-Vertrag (`IRenderer.h`): Pool, Tabelle je Consumer, Tausch je Landschaft, `supportsRvt` zuletzt. Eine Seite ist in allen dreien der **allgemeine** `RenderSceneImage`-Weg (rohe Ausgabe) und wird danach in ihre Scheibe kopiert, ohne Rücklesen; der schlanke Seiten-Pass von D3D11 (0,05 ms) ist nicht nachgebaut.

| | D3D12 | Vulkan | OpenGL |
|---|---|---|---|
| Bindung | Root-Tabelle +4 Slots (`kSrvPerDraw` 17 -> 21, Bereich t40..t43), Pool-Views in der Material-Schablone, die Tabelle des Consumers wird je Draw in den Block kopiert | vier Zeilen (40..43) hinter den Cluster-Listen in Set 0; nur ein Draw mit Tabelle schreibt sie (kein anderer Pipeline-Pfad benutzt sie) | Einheiten 21..24, nach Namen zugewiesen (`setupProgram`), je Draw an den zwei opaken Stellen (vorwärts, G-Buffer) |
| Pool | 3 Arrays, BaseColor `R8G8B8A8_TYPELESS` + sRGB-View | 3 Bildarrays, BaseColor UNORM mit SRGB-View (veränderliches Format) | 3 `GL_TEXTURE_2D_ARRAY`, BaseColor `SRGB8_ALPHA8` |
| Kopie | `CopyTextureRegion` | `vkCmdCopyImage` | `glBlitFramebuffer`, **vertikal gespiegelt** (GL speichert unten zuerst, der Pool oben zuerst), `FRAMEBUFFER_SRGB` aus |
| Tabelle | `R32_UINT` mit Mips, SRV in CPU-Heap | `R32_UINT` mit Mips, nearest-Sampler | `R32UI` mit Mips, nearest (sonst unvollständig) |
| Grenze | keine | Gerät muss 21 Fragment-Sampler tragen (`rvtFit`), sonst Layout ohne die Zeilen, `supportsRvt` aus; `HE_VK_MAX_STAGE_SAMPLERS` läuft den Fall ab | 25 Fragment-Textureinheiten (macOS-GL hat 16: aus, geloggt) |

Geprüft (RTX 4070, echte Geräte, Release-Editor, dieselben Zeugen wie bei D3D11, `HE_DUMP_RHI=D3D12|Vulkan|OpenGL`):

| | D3D11 | D3D12 | Vulkan | OpenGL |
|---|---|---|---|---|
| Scheiben gegen Rücklesen (`bakeRect`) | 0/27, größte Abweichung 1/255 (so seit 2b: der schlanke Pfad schreibt direkt in 8 Bit, der Positionstest ist das Orakel) | 27/27 | 27/27 | 27/27 |
| Positionsebenen 0..2, falsch > 3/255 | 0 | 0 | 0 | 0 |
| Negativkontrollen `half`/`flipz`/`swapxz` | gefangen | gefangen | gefangen | gefangen |
| Verbraucher (3 Sichten) + Feinmuster, falsch | 0 | 0 | 0 | 0 |
| Lit-Vergleich gegen das eigene Material, Mittel /255 | 1,5 | 1,51 | 1,50 | 1,52 |
| Treiber: Füllung / Leerlauf / Sculpt / Mal / Pfade | 3 Fr., 21 Pages / 0 / 7=7 / 6~6 / 1,47 und 1,51 | gleich | gleich | gleich |
| 2 x 2 Kacheln: Naht-Streifen / Inneres | 2,60 / 3,36 | 2,60 / 3,36 | 2,39 / 3,25 | 2,43 / 3,30 |
| Seite im Zeugenlauf (ms je Seite) | 0,6 allgemein, 0,05 schlank | 17 | 24 | 3,4 |

Der Zählerstand der Kacheln (4/4 nach 11 Frames, Striche 6 = 6 und 10 = 10, Kachel zerstört 84 -> 63, hin und zurück 16 -> 16 Materialien, Pool von 40: 0 Verdrängungen) ist in allen vier Backends gleich. Die Negativkontrolle zum Vulkan-Fall: mit `HE_VK_MAX_STAGE_SAMPLERS` 18 und 20 steht "Runtime Virtual Textures off" im Log und die Zeugen überspringen (`supportsRvt` aus), mit 21 läuft es. Der Software-Renderer meldet `supportsRvt` nicht; Treiber und Zeugen laufen sauber leer (der Treiber-Zeuge hat dabei einen Absturz gezeigt, den es nur bei diesem Aufruf ohne Fähigkeit gab: er prüft jetzt `supportsRvt` und sagt, dass er übersprungen wird).

Tests: die Sweeps in `test_material_graph.cpp` sind **streng** geworden. Der RVT-Knoten muss von der D3D12-Root-Signatur und vom Vulkan-Layout ganz bedient werden; die Signatur ohne den neuen Bereich und das Layout ohne Zeile 40 sind die Negativkontrollen; `rvtFit` und die Sampler-Zahlen (21/20) sind geprüft. `he_tests` (Debug, eigenes `APPDATA`): 535 Fälle der renderernahen Dateien grün, darunter die D3D12-WARP-PSO-Tests gegen die größere Signatur.

Nicht geprüft / bekannte Grenzen:
* **Metal ist nicht portiert** (`supportsRvt` bleibt falsch, die Landschaft zeichnet wie bisher). Ich habe es nicht blind geschrieben: es lässt sich hier weder übersetzen noch laufen lassen (nur im CI-Schritt nach ctest), und `MetalRenderer.mm` hat seine eigene Bindungs- und Shader-Spiegelung. Nötig: Pool als `MTLTexture` 2D-Array, Tabelle je Consumer, Bindung an Fragment-Texturen, Tausch nach der Extraktion, Seite per Blit in die Scheibe; die MSL kommt aus derselben Querübersetzung (`RvtLookup.h`).
* **Der Seiten-Pass ist in D3D12, Vulkan und OpenGL der allgemeine Weg** (siehe 6b unten für die Klammer, die ihn im Editor um eine Größenordnung billiger macht); ein wirklich schlanker Pass wie der von D3D11 (persistentes Seitenziel, kein Post-Chain) ist es nicht.
* Der Editor-Loop und das Spiel wurden nur auf D3D11 gelaufen (Schritt 4); auf den anderen drei läuft nur der Zeugen-Aufruf des Treibers (`updateTerrains`, dann `tick`).
* OpenGL ohne 25 Textureinheiten (macOS) und Vulkan-Geräte am Sampler-Minimum: nur über die Schalter nachgestellt (`HE_VK_MAX_STAGE_SAMPLERS`), nicht auf echter Hardware.
* CI (Linux/macOS/Windows) für Schritt 6 steht aus.

## Schritt 6b: eine Seitenklammer je Tick (2026-11-04)

**Befund (gemessen, vor der Änderung).** Die Zeugenläufe des Schritts 6 hatten keinen lebenden Viewport; im echten Editor (MCP-Brücke, `HE_RVT=1`, Release, RTX 4070, 256-m-Landschaft, `RHI` in der Konfiguration) kostet eine Seite im allgemeinen Pfad weit mehr, weil `RenderSceneImage` je Aufruf den lebenden Viewport beiseitelegt, einen Satz in Seitengröße baut (GPU-Flush, Post-FX-Ziele) und danach die Zwischenziele auf die Größe des Viewports zurückbaut. Der Treiber meldet jetzt `FrameStats::paintMs` (Wanduhr um das ganze Malen eines Ticks, in der Aktivitätszeile `painting took`):

| 8 Seiten je Tick (Füllung), 7 Seiten (Sculpt) | D3D11 | D3D12 | Vulkan | OpenGL |
|---|---|---|---|---|
| vorher: Füllung | 3,0 bis 3,7 ms | 263 bis 283 ms (34 ms je Seite) | 241 bis 259 ms (31 ms je Seite) | 31 bis 43 ms |
| vorher: Sculpt (8 + 3 Seiten, dann 7) | 2,4 + 0,7, 2,1 | 318 + 119, 199 | 428 + 143, 397 | 32 + 17, 32 |

**Änderung.** `IRenderer::BeginRvtPages(px)` / `EndRvtPages()` (Standard: nichts). `RvtLandscape::tick` ruft sie **einmal je Tick** um das ganze Malen aller Volumen (RAII, schließt auf jedem Weg). D3D12 und Vulkan halten dazwischen **einen** Seitensatz: die Aufrufe mit `rvtSlot >= 0` und passender Größe bauen ihn nicht neu, die Kopie in die Pool-Scheibe liegt in der Befehlsliste des Frames selbst (ein Submit, ein Warten; die Upload-Ringe verlangen das Warten), und der lebende Viewport kommt **einmal** am Ende zurück samt Reparatur der Zwischenziele (D3D12 `rvtStillEnter/Leave`, Vulkan `stillEnter/Leave`; der Einzelaufruf-Pfad ist dieselbe Funktion mit Enter und Leave um einen Aufruf und bleibt Rückfall und Referenz). `Render()` und `CaptureViewport()` von außen holen einen gehaltenen Satz zuerst zurück. D3D11 (schlanker Pass) und OpenGL (der Satz bleibt ohnehin in Seitengröße, Reparatur beim nächsten Frame) behalten die leeren Standardfunktionen.

**Der entscheidende Test** (`HE_DUMP_RVTTEST`, Teil 1b): dieselben Seiten werden einmal im Einzelpfad und dann in einer Klammer gemalt, in Slots +100 (vorher leer, damit eine Klammer, die nichts malt, nicht durch alte Seiten besteht); alle drei Layer **byte-identisch**: D3D12 27 von 27, Vulkan 27 von 27, 0 abweichend, 0 leer (auf D3D11/OpenGL trivial identisch, derselbe Pfad). Alle Zeugen wie in Schritt 6 danach unverändert (Negativkontrollen, Lit 1,51 / 1,50, Treiber, 2 x 2 Kacheln). Im Zeugenlauf (warm): D3D12 10,4 -> 1,1 ms je Seite, Vulkan 11,4 -> 0,8 ms.

| Editor, nachher | D3D12 | Vulkan |
|---|---|---|
| Füllung (8 Seiten) | 19 bis 24 ms im ruhigen Lauf | 21 bis 31 ms |
| Sculpt (8 + 3 Seiten, dann 7) | 39 + 12 ms, 47 | 47 + 28 ms, 68 |

Das sind rund **10x** weniger (D3D12 ~3 ms je Seite, Vulkan ~3 ms; D3D11 0,45 ms). **Die Messung ist verrauscht:** während der Läufe lief auf derselben GPU ein Spiel (VALORANT), die Läufe streuten für dieselbe Arbeit zwischen 20 ms und 350 ms; angegeben ist der ruhigste Lauf, die vorherigen Werte waren stets mindestens so hoch (ein Lauf je Backend, ebenfalls unter Last). Für eine belastbare Zahl bräuchte es eine freie GPU und mehrere Läufe.

Nicht gemacht: ein wirklich schlanker Pass (kein Post-Chain, ein persistentes Seitenziel, wie D3D11) und das Zusammenfassen der drei Layer einer Seite in einen Submit; dort liegt der Rest (~3 ms je Seite gegen 0,45).

## Schritt 6c: der schlanke Seiten-Pass für D3D12/Vulkan: gemessen und nicht gebaut (2026-11-04)

Vor dem Bau gemessen, wo die Zeit einer Seite nach der Klammer (6b) liegt (D3D12, Zeugenlauf ohne Last, je Layer einer Seite, vorübergehende Zeitnahme, nicht im Code):

| | je Layer |
|---|---|
| `DrawViewportFrame` aufzeichnen, davon `DrawScene` | 0,31 ms, davon **0,25 ms** |
| davon die Post-Kette (`runPostFX`) | **0,011 ms** |
| Kopie in die Scheibe + Schließen + Submit | 0,03 ms |
| Warten auf die GPU | 0,13 ms |

Ein Seiten-Pass ohne Post-Kette und ohne HDR-Ziel spart also **nicht die Kosten, die da sind**: die Kette ist fast gratis, die Zeit steckt im Aufzeichnen von `DrawScene` selbst. Dafür müsste D3D12 eine sRGB-Variante der Material-PSOs (RTV-Format steckt im PSO und im Schlüssel, an mehreren Stellen berechnet), einen Verzicht auf den Himmel und einen eigenen Seitenpfad bekommen, für bestenfalls ein Drittel von 0,5 ms je Layer. Nicht gebaut. Vulkan hat keinen Grund für einen anderen Schluss (0,8 ms je Seite im Zeugenlauf, dieselbe Aufteilung ist dort nicht eigens gemessen).

Was gebaut ist: der Schlüssel des Materials (Hash, Fragment-Quelltext, Vertex-Rumpf, Maske der Textur-Arrays) wird in D3D12 wie schon bei D3D11 (`matKeyCache`) **einmal je `DrawScene`-Aufruf je Material** gebildet (`MatKey`, `drawSceneSerial`), nicht je Draw: `resolveShaders` kopiert den ganzen Quelltext und hasht ihn, `matGlslTextureArrayMask` durchsucht ihn, bei jedem Chunk-Draw. Gilt für jedes Graph-Material, nicht nur RVT. Wirkung klein, aber real: 1,1 -> 0,9 ms je Seite im Klammer-Lauf. Vulkan memoisiert schon (`matFwdMemo`). Unverändert danach: alle D3D12-Zeugen (Scheiben 27/27 und Klammer 27/27, Positionsebenen 0 falsch, Verbraucher 0 falsch, Lit 1,51, Treiber, Kacheln), 300 Fälle `he_tests` (Culling, Material-Graph, Engine-Materials, RVT, D3D*, Deferred, Render) grün.

Der Rest (~0,9 ms je Seite im Zeugenlauf, ~3 ms im Editor unter Last) ist das Aufzeichnen und Absenden von `DrawScene` je Layer und das Warten der Upload-Ringe wegen; was ihn senken würde, ist die Walk-Wiederverwendung des Extraktors über die drei Layer einer Seite (D3D11 macht das in `RenderRvtPage`) oder ein Submit für die drei Layer, beides braucht Ring-Offsets je Layer. Im Editor kommt dazu, dass das erste Warten eines Ticks auch die noch laufenden echten Frames der GPU abwartet.

## Schritt 6d: die Layer einer Seite ohne Warten, je auf einem eigenen Frame-Slot (2026-11-04)

Aus 6c blieb: je Layer ein Abschicken **und ein Warten** (0,13 ms GPU-Wartezeit je Layer im Zeugenlauf, im Editor zusätzlich die noch laufenden echten Frames). Ein Befehlslisten-Submit für alle drei Layer geht nicht, weil jeder `DrawScene`-Aufruf die Ring-Zähler der Frame-Slots wieder auf 0 setzt und der zweite Layer die Slots überschriebe, die der erste noch nicht gelesen hat. Die vorhandene Lösung für genau dieses Problem ist der Frame-Slot: alle Ringe, Deskriptorblöcke, Beleuchtungspuffer und Pools sind je Slot angelegt, damit echte Frames überlappen dürfen (D3D12 `kFramesInFlight` 3, Vulkan `k_maxFramesInFlight` 2).

Gebaut (in der Klammer aus 6b, nur dort): der k-te Layer ohne Warten läuft auf Slot `(Basis + k) % Slots`, wird **abgeschickt, ohne zu warten** und gibt sofort zurück; gewartet wird, wenn alle Slots benutzt sind, und beim Schließen der Klammer (`stillLeave` wartet ohnehin). Der gemeinsame Zielsatz braucht nichts: eine Queue führt die Listen in der Reihenfolge aus, in der sie aufgezeichnet wurden, und die CPU-Zustände (Layouts, Ressourcenzustände) laufen in derselben Reihenfolge weiter. Vulkan hebt die Befehlspuffer bis zum Warten auf (`m_rvtPendingCmds`) und gibt sie dann frei. Schalter `HE_RVT_PIPELINE=0` wartet wieder nach jedem Layer (A/B und Referenz).

Geprüft: dieselben Seiten im Einzelpfad und in der pipelinierten Klammer (Teil 1b): **27 von 27 byte-identisch** in 10 von 10 Läufen je Backend (die Prüfung wäre bei einem Hazard nichtdeterministisch rot geworden), alle Zeugen danach unverändert (Negativkontrollen, Lit 1,51 / 1,50, Treiber, Kacheln).

| Seite in der Klammer, Zeugenlauf (5 abwechselnde Läufe, Rechner unter Last) | wartend | pipeliniert |
|---|---|---|
| D3D12 | 4,2 ms | **1,9 ms** (2,2x) |
| Vulkan | 2,4 ms | **1,55 ms** (1,6x) |

Editor (`RHI` in der Konfiguration, 256-m-Landschaft, 8 Seiten je Tick, ein Lauf je Modus, Rechner unter Last, verrauscht): D3D12 Füllung 44 -> **29 ms** je Tick (-32 %), Sculpt 63 -> 31 ms und 21 -> 10 ms; Vulkan Füllung 35 bis 41 -> **31 bis 34 ms** (-15 %), Sculpt 64 und 68 -> 68 und 60 ms, also **kein messbarer Gewinn** beim Sculpt (ein Lauf je Modus, Streuung größer als der Unterschied). Im Editor bleibt Vulkan damit bei ~4 ms je Seite.

Nicht gemacht: mehrere Seiten ohne Wartepunkt (mit 3 Slots passen genau drei Layer, also eine Seite), und die Walk-Wiederverwendung des Extraktors über die drei Layer.

## Schritt 6e: Walk-Wiederverwendung über die drei Layer einer Seite: gebaut, gemessen, verworfen (2026-11-04)

D3D12 hat den Extraktor nie in Klammern gesetzt, Vulkan nur je Aufruf. Gemessen: `extract()` kostet in D3D12 **0,165 ms** je Layer (von 0,25 ms `DrawScene`) auf einer Welt mit vier Objekten, die drei Layer einer Seite sehen aber dieselbe Welt von derselben Kamera (der Rahmenschlüssel des Extraktors vergleicht die RVT-Felder der Kamera nicht). Gebaut wie in D3D11 (`RenderRvtPage`): Layer 0 `beginFrame()`, Layer 2 `endFrame()`, und `endFrame()` auch beim Schließen eines gehaltenen Satzes; Vulkan ersetzte dafür den Ein-Aufruf-Bereich durch einen über die Layer; Schalter `HE_RVT_WALKREUSE=0` zum Vergleich.

Es wirkt: 18 von 18 wiederverwendbaren Walks wurden wiederverwendet (voll 136 -> 118, wiederverwendet 0 -> 18 über den Zeugenlauf), die Seiten bleiben **27/27 byte-identisch**. Aber der Seitenpreis bewegt sich nicht messbar: 4 abwechselnde Läufe je Backend (Rechner unter Last), D3D12 28,0 gegen 28,8 ms je neun Seiten (-3 %), Vulkan 23,1 gegen 23,4 ms (-1 %); die CPU-Zeit liegt bei den pipelinierten Layern nicht auf dem kritischen Pfad. **Nicht behalten**: kein belegter Gewinn, dafür ein Extraktor, der über Aufrufe hinweg geöffnet bleibt (Fehlerpfade, `stillLeave`). Wer sie braucht (etwa eine Welt mit vielen Objekten statt vier), findet den Stand in der Notiz hier; der Schlüsselvergleich muss dafür nicht angefasst werden.

## Schritt 7: die Einstellung im Editor, Speichern, Statusansicht (2026-11-04)

Der Treiber lief bisher nur mit `HE_RVT=1`. Jetzt ist es eine **Projekteinstellung**: `Config/ProjectSettings.json`, Abschnitt `rvt`, derselbe Ort wie die Schatten (Editor und exportiertes Spiel lesen ihn; der Export kopiert die Datei unverändert).

| Teil | Entscheidung |
|---|---|
| Einstellung | `HE::ProjectRvtSettings` (`ProjectSettings.h`): `enabled` (**aus**, Standard = heutiges Verhalten), `poolPages` 512 (108 MB), `metersPerTexel` 0,25, `nearRadiusM` 64, `activationRadiusM` 600, `maxPagesPerFrame` 8; geklemmt (`kMin*/kMax*`). Fehlender Abschnitt = Standard, also lädt eine Datei von vor Schritt 7 mit RVT aus; `isDefault()` sieht jedes Feld (ein Projekt, dessen einzige Änderung "RVT an" ist, bekommt seine Datei). |
| Wer liest | `RvtLandscape::wanted(project)` und `settingsFromProject(project)`: **`HE_RVT=1` / `0` in der Umgebung gewinnen über die Datei** (jedes Zeugenskript und die MCP-Schleife hängen daran), sonst gilt `enabled`. Editor und Spiel prüfen es **jeden Tick** (kein gemerkter `static`): ist es aus, ruft der Editor `detach()` (Tausch aus, Pool frei) und setzt den Treiber zurück, die Landschaft zeichnet wieder ihr eigenes Material. |
| Live oder Neuaufbau | `RvtLandscape::setSettings` baut **nur** neu (`detach()`, nächster Tick hängt wieder an), wenn sich Poolgröße oder Texelgröße ändern; Radien und Seitenbudget gelten sofort (`RvtManager::setMaxRendersPerFrame`). Im Editor sind Poolgröße und Texelgröße Auswahllisten (ein Ereignis je Änderung, mit MB daneben), die Radien werden erst beim Loslassen übernommen: kein Neuaufbau je Ziehbild. |
| Seite | Projekteinstellungen ▸ World ▸ **Virtual Texture** (`ProjectSettingsPanel::Page::VirtualTexture`, `EditorSettingsPanel::Page::WorldVirtualTexture`): der Schalter, Pool, Streaming, Hilfeeinträge für alle sechs Steuerungen (`EditorHelp.cpp`, `editor_help_audit.py --check` grün). Sie sagt, wenn das Backend es nicht kann (`Capabilities::supportsRvt`), und wenn `HE_RVT` den Schalter überstimmt. |
| Statusansicht | unter den Einstellungen: Landschaften aus Pages gezeichnet / bekannt, Pool belegt / Größe / veraltet, eine amber Zeile bei `starved` ("Pool zu klein"), die letzte Malzeit (`paintMs`) und eine **Seitenkarte** des feinsten Levels der nächsten Landschaft (grün = gemalt, orange = veraltet, dunkel = fehlt). Über `AppContext::rvtDebugInfo`, einen **Schnappschuss** (`RvtLandscape::debugInfo()`, kopiert, nichts zeigt in den Treiber): der Editor setzt den Treiber beim Ausschalten zurück, ein Zeiger würde hängen. |

Geprüft:
* `test_project_settings`: Standard aus, Round-Trip aller Felder, eine Datei **ohne** `rvt` lädt als aus (auch über einen vorher eingeschalteten Zustand), `isDefault()` wird falsch, wenn nur `enabled` sich ändert, Klemmung und falsche Typen. Die bestehende Feldliste (`HE_FIELD`) kennt die sechs Felder.
* `test_project_settings_ui`: die Seite zeichnet über einem echten Projekt und **Anschauen schreibt nichts**; ein Klick auf den Schalter schreibt `Config/ProjectSettings.json` und ein frisch geladener Manager liest `enabled` zurück; mit einem Schnappschuss (8 x 8 Karte, 15 grüne und 1 orange Zelle) zeichnet die Statusansicht diese Farben, ohne ihn keine (Bild: HE_UI_DUMP_DIR, `project-settings-virtual-texture*.bmp`).
* Echter Editor (D3D11, MCP-Brücke, Scratch-Projekt, `HE_RVT` **nicht** gesetzt): ohne Datei startet der Treiber nie (20 000 Frames, keine `RvtLandscape`-Zeile); mit `rvt.enabled: true`, Pool 256, Texel 0,5 m, Budget 4 in der Datei hängt er an (Pool "256 pages (54 MB)", "4 painted per frame", 3 Mips, 9 Pages), füllt in 3 Frames und malt einen Sculpt-Strich nach; mit `HE_RVT=0` über derselben Datei startet er nicht.
* `he_tests` (Debug, eigenes `APPDATA`) für Einstellungen, Projekt, Hilfe, RVT, Export, Szene, Landschaft, HLOD, Culling und Material-Graph: **742 Fälle, 741 grün**. Rot ist `test_hc_codegen_export_build` (die Codegen-Klassen bauen gegen das SDK: `vi.size() == vc.size()`, 6 gegen 0, leere Fehlerzeile); er war schon in Läufen **vor** Schritt 7 rot (auch bei der zweiten Session), also nicht von diesem Schritt, **aber nicht auf dem alten Stand nachgewiesen**.

Nicht gebaut / nicht geprüft:
* **Kein Ausschalten im laufenden Editor gesehen**: der Pfad (`wanted` falsch -> `detach()` + Zurücksetzen) ist klein, aber es gibt kein MCP-Werkzeug, das die Einstellung zur Laufzeit umlegt; gesehen sind nur "Datei aus", "Datei an", "`HE_RVT=0` über an".
* **Das Spiel** (`GameApplication`) liest die Einstellung jetzt aus seinen geladenen `ProjectSettings`, ist übersetzt, aber **nicht gelaufen** (auch nie mit RVT; ein exportiertes Spiel zu starten braucht den Export).
* **Keine Einstellung je Landschaft** (`TerrainComponent`): sie bräuchte eine Änderung am `SceneSerializer` samt Test, dass alte Szenen weiter laden; die Szene bleibt unberührt (der Tausch schreibt nie in die Welt), also ist das Speichern der Szene schon richtig. Ein Opt-out je Landschaft ist ein eigener Schritt.
* Die Statusansicht ist eine Seite der Projekteinstellungen, keine Überlagerung im Viewport; die Seitenkarte zeigt nur das feinste Level einer Landschaft.
* Der Abschnitt "Project: virtual texture" im Settings Reference (das Hilfe-Handbuch) ist als Zuordnung eingetragen; der Text dazu stammt aus den Hilfeeinträgen.

## Schritt 8: Abnahme (2026-11-04)

**Aufbau.** Das **exportierte Spiel** (Projekt-Export, Profil Development, Host), nicht der Editor: 1024-m-Auto-Landscape (256 Chunks, 69 Draws im Bild), Projekteinstellung `rvt.enabled: true` mit den Standardwerten (512 Pages, 0,25 m), je Lauf `HE_PROFILE_CAPTURE=600` nach 900 Warmup-Frames (vsync aus, `HE_HIDDEN_WINDOW`), Fenster 1920 x 1080 in der Konfiguration = **2400 x 1350 Pixel** (DPI 125 %), RTX 4070, freie GPU (kein Spiel im Hintergrund, anders als bei 6b-6d). `HE_RVT=0` / `1` schalten um, drei Läufe je Zelle, Mediane. Die Skripte stehen in `docs/rvt-acceptance-2026-11-04/` (Export über die MCP-Brücke, `run_game.ps1`, `matrix.ps1`, `summarize.py`; sie setzen die Einrichtung des Autors voraus: kurzer `APPDATA`-Pfad für den SDK-Build, MSVC-Umgebung, ein Projekt mit `Content/Scenes/Big.hescene`).

| Rechner-Mediane, 600 Frames | Frame ms aus | Frame ms an | GPU-Frame ms aus / an | Scene-Pass ms aus / an | VRAM-Zuwachs aus / an |
|---|---|---|---|---|---|
| D3D11 | 1,628 | **1,556** (-4 %) | 0,818 / 0,814 | 0,225 / 0,222 | +326 / +525 MB |
| D3D12 | 1,127 | **1,123** (0 %) | 1,072 / 1,074 | 0,198 / 0,199 | +489 / +624 MB |
| Vulkan | 1,440 | **1,421** (-1 %) | 1,131 / 1,061 | (nicht aussagekräftig, 0,01) | +598 / +879 MB |

(VRAM: `nvidia-smi` gesamt belegt, Spitze während des Laufs minus vor dem Start; systemweit, aber der Rechner war sonst ruhig, die Streuung zwischen den Läufen 0 bis 1 MB, außer D3D12 an: 546 gegen 624.) Ein zweiter Satz mit 3072 x 1728 in der Konfiguration ergab dasselbe Bild (D3D11 1,700 -> 1,636, D3D12 1,199 -> 1,186, Vulkan 1,407 -> 1,397 ms): **das Fenster wird auf die Anzeige begrenzt (2560 x 1380), es ist kein 4K-Lauf**. Die Seiten füllen sich in den ersten Frames (8 je Tick: D3D11 164 ms für den ersten, danach 1 bis 2 ms; D3D12 562 ms für den ersten, danach 12 bis 22 ms) und danach meldet der Treiber **keine Aktivität** in den 600 aufgenommenen Frames: das Material läuft nicht mehr, solange sich nichts ändert.

**Befund, ehrlich.**
* **Kein messbarer Gewinn an GPU-Zeit in dieser Szene (siehe 8b: die Szene taugte nicht, mit dem Mix-Material gibt es 25 bis 53 %).** Das ganze GPU-Frame ist ~1 ms, die Landschaft ~0,2 ms davon; der Frame ist **CPU-begrenzt** (Frame-Zeit ~ CPU-Zeit: 1,6 gegen 0,8 ms GPU auf D3D11). Der 4-%-Unterschied auf D3D11 ist CPU-Zeit, nicht GPU-Zeit (GPU 0,818 gegen 0,814). Der Entwurf zahlt sich nur aus, wenn das Landschaftsmaterial teuer ist oder die GPU schwach; **das ist hier nicht gezeigt**. Die einzigen Zahlen mit Gewinn sind die Zeugen-Aufnahmen mit der Landschaft im ganzen Bild (Lit-Kosten, Schritt 3d, 6: D3D12 8,74 -> 8,02 ms, Vulkan 27,2 -> 25,4 ms, OpenGL 2,34 -> 1,67 ms je Aufnahme inklusive Rücklesen), und die sind eine andere Messung.
* **Speicher: der Preis ist 135 bis 280 MB** (D3D11 +199, D3D12 +135, Vulkan +281 MB) bei der Standard-Pool-Größe, deren Pool allein 512 x 0,222 = 114 MB sind. Der Rest ist Kopie der Szene (Vermutung: die Chunk-Meshes der Landschaft liegen ein zweites Mal im Scratch-Welt-Renderer; **nicht gemessen**, nur aus der Differenz geschlossen), Tabelle, Seitenziele, auf Vulkan der Seitensatz.
* **Mit der echten Albedo-Textur-Array-Datei der Engine** (`T_Landscape_Albedo_Array`, aus dem EngineContent-Cache kopiert; Normal- und Mask-Array fehlen dort) wird der Unterschied größer: +477 / +335 / +559 MB, und **Vulkan ist mit RVT an um 5 % langsamer** (1,372 -> 1,435 ms, drei Läufe je Seite, 1,360 bis 1,374 gegen 1,433 bis 1,439). Ursache nicht untersucht; aber der Lauf "aus" lädt das Array gar nicht (kein Fehler in dessen Log), "an" lädt es, weil die Ansichtsgraphen des Bakers es abtasten: **die beiden Läufe zeichnen dann nicht dasselbe**, der erste Satz (ohne Array, in beiden gleich ohne) ist der gleichwertige Vergleich.
* **Das Spiel läuft mit der Einstellung** (erstmals überhaupt mit RVT): der exportierte Build liest `Config/ProjectSettings.json`, hängt den Treiber an ("1024 m landscape, 6 mips"), füllt, zeichnet "1/1 segment(s) drawn from pages" und beendet sich sauber, auf **D3D11, D3D12 und Vulkan**. OpenGL (Spiel) und Metal nicht.
* Ein Fehler im Zeugen-Aufbau, kein Engine-Fehler: eine Szene, die ein **nicht auf der Platte liegendes Material** nennt, hängt den Treiber nie an (`not attached (material not resident)`, einmal geloggt, danach still). Eine Wiederholung für "Material lädt noch" habe ich gebaut, gesehen, dass es nicht das war, und wieder entfernt.

**Zwischenfall beim Messen (Werkzeug, nicht Engine).** Der gemeinsame Build-Baum `out/build/x64-release` wurde um 22:56 mit Standardoptionen neu konfiguriert (`DEPLOY_DIR` = `HorizonEngineBuild`, `HE_SDK_RELEASE_SET=ON`); meine Builds danach (22:58 bis 23:05) haben ein **Release-Build des RVT-Zweigs in den Deploy-Ordner des Nutzers installiert** (Editor, Game, EngineSDK dev + release). Geprüft: alles Release (keine `*D`-Laufzeit), nichts lief von dort. Danach habe ich einen eigenen Baum `out/build/rvt-rel` mit `-DDEPLOY_DIR=out/deploy-rvt-rel -DHE_ENGINE_SDK_DIR=... -DHE_SDK_RELEASE_SET=OFF` benutzt und in `x64-release` nicht mehr gebaut.

Nicht geprüft: OpenGL im Spiel, Metal, eine teure/mehrschichtige Landschaft mit vollständigem EngineContent (die Szene hier ist die Zeugen-Auto-Landscape), 4K-Auflösung (Fenster auf die Anzeige begrenzt), mehrere Landschaften im Spiel.

## Schritt 8b: die Abnahme mit dem teuren Material wiederholt (2026-11-04)

**Warum Schritt 8 falsch lag.** Der Satz "kein messbarer Gewinn" oben gilt nur für die dortige Szene, und die war kein Test des Entwurfs: (1) die Szene hatte **weder Sonne noch Himmel** (das Landschaftsbild war schwarz), (2) die Kamera stand bei y = 2 **unter** der Landschaft (y = 300), das Bild zeigte fast keine Landschaft (Scene-Pass 0,2 ms von 1 ms), (3) das Material war eine Instanz der Mountains-Gruppe, **nicht das Mix-Material mit mehreren Gruppen**, das der Entwurf eigentlich ersetzen soll, (4) die Frame-Zeit war CPU-begrenzt. Das Mix-Material ist außerdem **nie von RVT bedient worden**: `HlodBaker::begin` lehnte es ab (`autoLandscapeGroupOfMaterial(Mix)` = -1, "not an Auto Landscape material"), eine Mix-Landschaft hätte also im Spiel den vollen Shader behalten, ohne dass etwas es meldete. Das ist behoben (`LandscapeHlodBaker.cpp`: für `kAutoLandscapeMixMaterialId` gilt `TerrainComponent::baseGroup`, und die drei Ansichten werden mit `mixed = true` gebaut; die Gruppenanteile in den Gewichtsebenen 8..11 kommen mit der `TerrainComponent`-Kopie in die Scratch-Welt).

**Aufbau.** Wieder das exportierte Spiel (Development, RTX 4070 frei, 2400 x 1350 wegen 125 % DPI, vsync aus, 900 Warmup + 600 Frames, Median aus drei Läufen, `HE_RVT=0/1`). Szene: 1024 m, Hügel (-14..82 m), **Mix-Material** (`M_AutoLandscape_Mix`, zwei stärkste Gruppen je Fragment, Schnee/Sand prozedural, Texture Bombing an), **alle vier Gruppen großflächig gemalt** (Gewichtsebenen 8..11, 256 x 256), Sonne und Himmel (Sky-Entity mit Standardwerten). Erzeugt von `gen_mix_scene.py` (liegt neben den anderen Skripten); drei Kameras, je mit einem Bildschirmfoto geprüft: **schräg** (140 m über dem Gelände, 32 Grad nach unten, Landschaft füllt mehr als die untere Bildhälfte), **am Boden** (60 m über dem Gelände, 12 Grad, Blick über die Hügel), **von oben** (500 m, Landschaft füllt das Bild).

| Szene | Backend | Frame ms aus -> an | GPU-Frame ms aus -> an | Scene-Pass ms aus -> an | VRAM-Zuwachs aus / an |
|---|---|---|---|---|---|
| schräg | D3D11 | 7,32 -> **3,71 (-49 %)** | 4,41 -> 2,39 | 2,62 -> 0,60 | +413 / +611 MB |
| schräg | D3D12 | 5,82 -> **4,10 (-30 %)** | 5,79 -> 4,07 | 2,71 -> 0,97 | +528 / +669 MB |
| schräg | Vulkan | 5,08 -> **2,92 (-42 %)** | 5,05 -> 2,89 | (nicht aussagekräftig) | +692 / +931 MB |
| am Boden | D3D11 | 8,60 -> **4,06 (-53 %)** | 4,78 -> 2,83 | 2,41 -> 0,44 | +398 / +597 MB |
| am Boden | D3D12 | 6,41 -> **4,79 (-25 %)** | 6,39 -> 4,75 | 2,44 -> 0,81 | +501 / +642 MB |
| am Boden | Vulkan | 5,01 -> **3,37 (-33 %)** | 4,98 -> 3,30 | (nicht aussagekräftig) | +665 / +891 MB |
| von oben | D3D11 | 3,49 -> 3,52 (0 %) | 2,33 -> 2,37 | 0,61 -> 0,65 | +403 / +599 MB |
| von oben | D3D12 | 2,92 -> 2,93 (0 %) | 2,89 -> 2,90 | 0,76 -> 0,77 | +522 / +665 MB |
| von oben | Vulkan | 2,89 -> 2,89 (0 %) | 2,86 -> 2,86 | (nicht aussagekräftig) | +646 / +927 MB |

Die Streuung zwischen den drei Läufen ist klein (D3D11 schräg: aus 7,25 bis 7,78, an 3,67 bis 3,71; Vulkan: aus 4,71 bis 5,36, an 2,92 bis 2,94). Die Zahlen kommen aus `summarize.py <Präfix>` über `matrix.ps1 -Prefix <Präfix>`.

**Befund.**
* **Mit dem teuren Material und einer Landschaft im Bild ist der Gewinn klar: 25 bis 53 % Frame-Zeit, auf allen drei Backends.** Auf D3D11 fällt der Scene-Pass von 2,6 auf 0,6 ms; das Seitenmaterial kostet pro Pixel nur noch die Abfrage der drei Seitentabellen. Im Lauf wird nichts neu gemalt (der Treiber meldet Leerlauf).
* **Von oben (500 m über dem Gelände) kein Gewinn, aber das ist kein Fehler des Entwurfs**: auch ohne RVT kostet die Landschaft dort nur 0,6 ms im Scene-Pass, das Material ist in der Ferne schon billig (nicht im Einzelnen untersucht). RVT bringt etwas, wenn das **nahe** Material teuer ist.
* **Die Texturlast ist zu niedrig angesetzt.** `T_Landscape_Mix_{Albedo,Normal,Mask}_Array` fehlen im Export ("in no packed root", als Null-UUID gebacken: alle Texturabfragen treffen eine Fallback-Textur, das Bild ist weiß). Die **Rechenarbeit des Shaders ist echt** (alle Gruppen, beide stärksten Gruppen, Bombing, prozeduraler Sand), aber die Bandbreite der drei fetten Arrays fehlt. Mit den echten Texturen kostet der volle Shader vermutlich mehr, die Seiten gleich viel: die Zahlen oben sind wahrscheinlich eine Untergrenze des Gewinns. **Nicht gemessen.**
* **Weiterhin der Preis: 140 bis 240 MB VRAM** (schräg: D3D11 +198, D3D12 +141, Vulkan +239 MB; der Pool allein 114 MB), unverändert zu Schritt 8.
* **Bildgleichheit:** der Lit-Zeuge (`HE_DUMP_AUTOLAND=1 HE_DUMP_AUTOLANDMIX=1 HE_DUMP_HLODBAKE=128 HE_DUMP_RVTTEST=1 HE_DUMP_RVTTEST_LIT=1`) mit Mix: mittlere Abweichung zwischen eigenem Material und Seiten **1,79/255**, größte 116, 1,61 % der Pixel weichen um mehr als 16 ab, auf D3D11 und D3D12 (Mountains allein: 1,55). Auch dort fehlten die drei Mix-Arrays, die Textur-Anteile am Bild sind also nicht mitgeprüft.
* **Ein Bildfehler, in beiden Läufen (aus und an) gleich, also nicht von RVT:** ein senkrechter Streifen bei x = 0 (blau), links davon strukturiertes, rechts davon einfarbig weißes Gelände. Wahrscheinlich eine Eigenheit meiner gemalten Gruppenanteile oder des Mix-Materials ohne Texturen; nicht untersucht.
* **Die Vulkan-Verlangsamung aus Schritt 8** (mit dem Albedo-Array 5 % langsamer) ist **nicht erneut gemessen**.
* D3D12 spart weniger (-25 bis -30 %) als D3D11 und Vulkan (-33 bis -53 %); Ursache nicht untersucht (D3D12 hat in der Szene mehr Draws, 315 gegen 135).

Nicht geprüft: echte Mix-Texturen im Export, schwächere GPU, OpenGL im Spiel, Metal.

## Handoff

- **Topic / step:** 333, Schritte 1 bis 8 gebaut und abgenommen **mit Einschränkungen** (5: Segmentierung als Stand-in für Tiles; 6: D3D12, Vulkan, OpenGL, **ohne Metal**; 7: Projekteinstellung, Seite, Statusansicht; 8: Abnahme am exportierten Spiel; 8b: mit dem Mix-Material und einer Landschaft im Bild **25 bis 53 % weniger Frame-Zeit auf D3D11/D3D12/Vulkan, 140 bis 240 MB VRAM Preis**; die Szene aus Schritt 8 allein (billiges Material, Landschaft kaum im Bild) war kein Test).
- **Branch / commit:** `claude/runtime-virtual-textures` (Stand der Schritte 1-4: siehe `git log`; Schritt 5: 5a 50707eb6, 5b-5d und Zeuge in den Commits danach). Der Branch enthält auch lokale Commits der zweiten Session.
- **Done:** CPU-Kern, Renderer-Vertrag + D3D11-Page-Pass, Consumer (Knoten, Lookup, Bindung), Treiber mit Journal und Tausch (echter Loop mit einer Landschaft geprüft), mehrere Landschaften in einem Pool (Zeuge `HE_DUMP_RVTTILES`), D3D12/Vulkan/OpenGL mit allen Zeugen grün (Schritt 6, Commits c2c9cd11, ab34291a, 09817cc1 und der Wächter in den Zeugen).
- **Files (neu in 5):** `HE_Scene/{include/HorizonScene,src}/RuntimeVirtualTexture.*` (`updateAll`, `missing`), `RvtLandscape.*` (Segmente), `HE_Core/include/Renderer/IRenderer.h` (Tabelle je Consumer, `SetRvtLandscape(entity, ...)`, `ReleaseRvtConsumer`), `HE_Rendering/.../D3D11Renderer.*` (`rvtTables`, `rvtSwaps`), `RenderObject::terrainEntity` + `RenderExtractor.cpp`, `MaterialGraph/RvtLookup.h` (Kantenklemmung), `HE_Editor/EditorApplication.cpp` (Zeuge `HE_DUMP_RVTTILES`, mit `HE_DUMP_AUTOLAND=1 HE_DUMP_AUTOLANDSIZE=128`), `tests/test_rvt_core.cpp` (+3 Fälle).
- **Zeugen-Schalter:** `HE_DUMP_AUTOLAND=1 HE_DUMP_HLODBAKE=128 HE_DUMP_RVTTEST=1` / `HE_DUMP_RVTDRIVER=1` / `HE_DUMP_RVTTILES=1` plus `HE_DUMP_AUTOLANDSIZE`, `HE_DUMP_RVTTEST_TEXEL`, `_PERF=<reps>`, `_FAULT=half|flipz|swapxz`, `_LIT=1`. Immer `HE_HIDDEN_WINDOW=1`, eigenes `APPDATA`, `HE_DUMP_RHI=D3D11`; Warnungen stehen auf stderr, Infos auf stdout.
- **Not verified:** CI für Schritt 7 und 8 (Linux, Linux-Vulkan, Windows zuletzt grün, macOS rot an `GpuArrayCook.cpp:170`, fremd), OpenGL im Spiel, Metal (nicht portiert), vollständiger EngineContent, mehrere Landschaften im Spiel, echte Tiles.
- **Traps:** **den gemeinsamen Baum `out/build/x64-release` nicht benutzen** (er wird von anderen neu konfiguriert und deployt dann in `HorizonEngineBuild`, siehe Schritt 8): eigener Baum (`cmake --preset x64-release -B out/build/rvt-rel -DDEPLOY_DIR=... -DHE_ENGINE_SDK_DIR=... -DHE_SDK_RELEASE_SET=OFF`) und `grep ^DEPLOY_DIR` vor jedem Build; ein Export baut das Spiel aus dem SDK und braucht MSVC in der Umgebung (`vcvars64`) und einen **kurzen** `APPDATA`-Pfad (MAX_PATH im SDK-Build); `HE_DUMP_AUTOLANDSAVE` speichert das Material nicht mit, eine gespeicherte Szene braucht ein Material, das auf der Platte liegt; `he_tests` mit **eigenem APPDATA**; `small` ist unter Windows ein Makro (`rpcndr.h`); vor einem Push auf fremde Commits achten (`git log origin/release/0.8.0..HEAD`); `tests/CMakeLists.txt` und `docs/img*` werden von der zweiten Session angefasst: nur eigene Hunks committen.
- **Next steps:** 1. Schritt 8b mit den **echten** Mix-Arrays wiederholen (Albedo/Normal/Mask im Export); die Vulkan-Verlangsamung aus Schritt 8 dort prüfen; eine schwächere GPU; den senkrechten Streifen bei x = 0 im Mix-Bild (auch ohne RVT) klären. 2. Den VRAM-Preis senken: die Scratch-Welt teilt die Chunk-Meshes mit der echten Landschaft nicht (Vermutung, erst messen). 3. Metal portieren (auf einem Mac laufen lassen, nicht blind). 4. Absprache mit Thema 326: `heRvtSample` in `kLandscapeFS`, je Tile ein Volumen; Tile-Laden/-Entladen als vierter Auslöser. 5. Optional: Opt-out je Landschaft (`TerrainComponent` + `SceneSerializer` + Test, dass alte Szenen laden), Seitenkarte als Viewport-Überlagerung, Wetter in die Scratch-Kopie.
