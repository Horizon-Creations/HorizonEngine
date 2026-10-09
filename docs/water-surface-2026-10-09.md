# Wasserfläche aus dem Wassermodell (Thema 174, Schritt 4)

Stand 09.10.2026, Mac, Debug-Build. Schritt 3 lieferte das Wassermodell
(`HE::water::Field`, Körper, coverage- und owner-Raster). Dieser Schritt macht daraus
sichtbares Wasser: je Körper ein flaches Mesh mit dem Engine-Wasser-Material (Thema 152),
als ganz normale Entity, die jedes Backend über den allgemeinen Mesh-Pfad zeichnet.
Nicht angefasst, wie vorgegeben: Ufer-Beschneidung (Schritt 5), Terrain-Aushub (6).

Code: `src/HE_Scene/include/HorizonScene/WaterMesh.h` (Geometrie, ausführlich kommentiert),
`src/WaterMesh.cpp` (Gitter, Konturen, Mesh), `src/WaterTriangulate.cpp` (Ear-Clipping),
`WaterSurface.h` + `src/WaterSurface.cpp` (Entities in der Welt),
`Components/WaterSurfaceComponent.h`, Tests in `tests/test_water_surface.cpp` (34 Fälle),
Witness `HE_DUMP_WATERLAKE` in `EditorApplication.cpp`.

## Ablauf in einem Absatz

Pro Körper: das Feld wird zu einem **Eckengitter** (an jeder Ecke zwischen vier Zellen der
Mittelwert ihrer coverage, nur vom eigenen Körper), darauf läuft **Marching Squares** bei
127,5 mit linear interpolierter Lage der Schnittpunkte, die Segmente werden zu **geschlossenen
Konturen** zusammengesetzt (Wasser links: Außenrand gegen den Uhrzeigersinn, Insel im Uhrzeigersinn),
jede Insel kommt zum kleinsten Außenrand, der sie enthält, und **Ear-Clipping mit Loch-Brücken**
macht aus Außenrand und Inseln Dreiecke. Das Mesh liegt bei y = 0 um die Mitte der Körperbox,
Normale +Y, UVs sind Weltkoordinaten. Die Entity steht bei `(Mitte.x, level, Mitte.z)` als Kind der
Landschaft.

## Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Ein Mesh pro Körper oder pro Chunk | **pro Körper** | Ein See ist eine Fläche mit einem Spiegel. Kein Chunk-Rand, an dem Konturen geschnitten werden müssen, keine Naht, an der ein Pixel durchscheinen kann (Wasser ist transluzent, die Terrain-Chunks verdecken ihre Naht mit Schürzen), ein Draw je See. Der Preis: ein Ozean ist ein Mesh und wird nicht teilweise gecullt. |
| Wo liegt der Schwellwert | **127,5** (`kContourIso`), nicht `kWet` (128) | Eine Kante, die auf dem Gitter liegt, gibt Eckenwerte von genau 127,5; dann geht die Kontur durch sie hindurch und die Polygonkante bleibt, wo sie gezeichnet wurde. `kWet` beantwortet "ist diese ZELLE Wasser", das ist eine andere Frage. |
| Dreiecke | Ear-Clipping, nicht eine Zelle je zwei Dreiecke | Etwa ein Dreieck je Konturpunkt statt je Zelle: der L-förmige See hat 10 Dreiecke, der schräge Stern des Witness (lange Diagonalen, ein Konturpunkt je Zelle) 675. Konkav und Löcher brauchen keinen Sonderfall. Der Triangulator ist nach mapbox/earcut (ISC) umgeschrieben, mit Z-Ordnung für große Ringe und den drei Eskalationen, wenn nichts mehr abgeschnitten werden kann. |
| Vereinfachung der Kontur | nur Duplikate und kollineare Punkte (Toleranz ein Tausendstel Zelle) | Douglas-Peucker mit echter Toleranz könnte zwei fast berührende Konturen kreuzen. Gerade Ufer kosten so trotzdem keinen Punkt je Zelle; ein gekrümmtes kostet einen je Zelle. |
| Ecken | eine Gitter-Ecke wird **diagonal abgeschnitten** (eine halbe Zelle Fläche) | Der Preis einer Gitterkontur. Ein achsparalleles Rechteck verliert so je konvexer Ecke 0,5 Zellen². Bei natürlichen Ufern nicht sichtbar, in den Tests als Toleranz eingerechnet. |
| UVs | `(terrain-lokales xz + Weltposition der Landschaft) / 1 m` | Zwei Körper (und zwei Landschaften) nebeneinander setzen das Muster fort. Das Wasser-Material leitet seine Wellen ohnehin aus `WorldPos.xz` ab; die UVs sind für eine spätere Textur da. Verschiebt man die Landschaft, werden die Meshes neu gebaut (nur Verschiebung, keine Drehung oder Skalierung). |
| Generierte Entity | eigene `WaterSurfaceComponent`, kein Missbrauch von `TerrainChunkComponent` | Die Terrain-Schleifen (Material-Sync, Chunk-Index, Tessellierung) würden Wasser-Chunks sonst als Terrain-Chunks lesen. |
| Wo übersprungen | **ein** Prädikat `HE::isTerrainGenerated` (und `terrainOwnerOf`) in `TerrainChunkComponent.h` | Serializer (2 Stellen + Subtree), Outliner, Netz-Replikation, Viewport-Aktionen, Brush-Snap, Rahmen-Alles, Verwaisten-Reinigung: alles, was Chunks übersprang, fragt jetzt das Prädikat. Ein neuer Typ generierter Kinder kommt dorthin und wird nicht an zehn Stellen vergessen. Ein Klick auf Wasser wählt die Landschaft, nie die versteckte Fläche. |
| Wo läuft der Neuaufbau | in `TerrainSystem::updateTerrains`, hinter der Terrain-Schleife, **außerhalb** des Dirty-Gates | Alle gut 15 direkten `updateTerrains`-Aufrufe des Editors und der Headless-Dump (rendert vor dem ersten Tick) bekommen es ohne weitere Verdrahtung; eine Wasseränderung lässt das Terrain sauber und braucht trotzdem ihre Fläche. |
| Material | `kEngineWaterMaterialId`, bei Bedarf per UUID, sonst per Pfad geladen | Der Headless-Dump kennt die UUID vor dem Scan der Engine-Inhalte noch nicht (der erste Witness-Lauf zeigte graues Ersatzmaterial; der Pfad-Fallback hat das behoben). |
| Schatten | wirft keinen (`castsShadow = false`) | Ein transluzentes Blatt würde eine harte graue Platte werfen. |
| Navigation | `collectStaticGeometry` überspringt Wasserflächen | Sonst wäre jeder See ein begehbarer Boden. |

## Wann wird was neu gebaut

Eine Wasseränderung setzt `Field::dirty` und das Rechteck. `WaterSurface::update`:

1. Eine Runde, in der nichts markiert ist, jede Fläche mit heutigen Parametern auf dem Spiegel
   ihres Körpers steht: Rückkehr nach ein paar Vergleichen (fast jeder Tick).
2. Sonst: ein Durchlauf über das Raster liefert die Zellbox jedes Körpers.
3. Ein Körper kommt in Frage, wenn er keine Fläche hat, sein Spiegel oder die Parameter der
   Landschaft (Größe, Auflösung, Weltposition) sich geändert haben, oder das um eine Zelle
   verbreiterte Änderungsrechteck seine Box (jetzt oder beim letzten Bau) trifft.
4. Von diesen bekommt nur der ein neues Mesh, dessen **Gitter** (Hash über Gitterwerte,
   Spiegel, UV-Optionen) sich wirklich geändert hat. Ein Pinselstrich in der Kerbe eines L-förmigen
   Sees baut den Teich, nicht den See.
5. Körper ohne Wasser (`clearBody`, `removeBody`) verlieren ihre Entity samt Mesh. `water.dirty`
   wird zuletzt zurückgesetzt.

Das Mesh wird einmal registriert und danach in place ersetzt (`replaceStaticMesh` +
`InvalidateMesh`; jede Registrierung kann den Pool des ContentManagers unter fremden Zeigern
bewegen). Ein Mesh, dessen Entity weg ist, geht zurück: das tut jeder Undo (baut alle generierten
Entities neu) und das Löschen einer Landschaft.

## Belege

Tests: `he_tests -tc='Water triangulate*,Water contours*,Water mesh*,Water surface*'`, 34 Fälle.

- **Triangulator** (jeder Fall prüft Fläche, Überlappung und Dreieckszahl `V + 2H − 2`): Quadrat,
  L-förmiges Polygon in beiden Windungsrichtungen (Kerbe unbedeckt), Sterne mit 5, 7 und 12 Spitzen,
  ein und zwei Löcher, Loch im Arm eines U (Brücke muss sehen), **300 zufällige einfache Polygone
  mit und ohne Loch** weit vom Ursprung, 600-Punkt-Ring mit Loch (Z-Ordnungs-Pfad), degenerierte Eingabe.
- **Konturen**: Sattel (Mittel der Ecken entscheidet), achsparalleles Rechteck (genau die acht Punkte,
  Fläche 478 von 480), **zwei getrennte Bereiche eines Körpers geben zwei Konturen**, Insel als Loch
  plus Teich auf der Insel als eigener Außenrand, Wasser am Terrain-Rand reicht bis zum Rand,
  Determinismus.
- **Mesh**: L-förmiger See (Fläche gleich Polygonfläche auf ein Prozent, keine überlappenden Dreiecke,
  alle Dreiecke +Y), Stern neben dem Gitter, Y = 0 und Normale +Y, UV = Weltkoordinate, Körper ohne
  Wasser, großer See (r = 190 Zellen) in guter Zeit.
- **Welt**: Entity mit Material, `castsShadow = false`, Eltern, Position; nichts geändert = nichts gebaut;
  Änderung baut nur den berührten Körper; Änderung nebenan, die den See nicht erreicht, baut ihn nicht;
  neuer Spiegel; entfernter Körper nimmt Entity und Mesh mit; `clearBody` und Neuraster unter derselben
  Id; verschobene Landschaft verschiebt die UVs; **nichts davon wird gespeichert und alles kommt aus
  dem Feld zurück**; Undo gibt die alten Meshes zurück; gelöschte Landschaft; Prädikat; verwaiste Kopie
  wird entfernt; Navigation; zwei Landschaften.

Bilder (`docs/water-surface/`), Debug-Editor, Himmel `HE_SKY_TIME=1.0`, `TOD=0.4`, Forward-Pfad,
AA aus, Kamera `CAMY=375 CAMZ=48 PITCH=-60`, Witness `HE_DUMP_WATERLAKE=<Form>`:

| Form | Metal | OpenGL | Dreiecke |
|---|---|---|---|
| L (nicht quadratisch, konkav) | `metal-l.png` | `opengl-l.png` | 10 |
| Stern (fünf Kerben, neben dem Gitter) | `metal-star.png` | | 675 |
| Ring (Insel, schräg, mit Teich darauf) | `metal-ring.png` | `opengl-ring.png` | 187 |
| zwei Seen, 0,8 m Spiegelunterschied | `metal-two.png` | | 258 |
| Kontrolle: Landschaft ohne Wasser | `metal-none-control.png` | `opengl-none-control.png` | |

Der Log des Dumps (`dump counters`) belegt: Kontrolle `draws=1 tris=8704`, L `draws=2 tris=8714`
(also genau die 10 Dreiecke des Sees dazu), zwei Seen `draws=3`. Die Wellen und die Schaumflecken
sind das Engine-Wasser, nicht das graue Ersatzmaterial; die Fläche steht unverändert auf beiden
Backends.

## Was die Fläche in Ruhe lässt

- **Physik**: Körper entstehen nur für Entities mit `RigidBodyComponent`, Charaktere und
  Landschaften (`PhysicsWorld::initialize`); die Fläche hat nichts davon, und ein Test lässt eine
  Kugel über einem See fallen: sie kommt am Boden an, nicht auf dem Wasser. Schwimmen und Auftrieb sind
  nicht Teil des Themas.
- **Navigation**: `collectStaticGeometry` überspringt die Fläche (sonst wäre der See ein Boden).
- **Landschaftspinsel**: er marschiert über das Höhenfeld (`TerrainTools.cpp` `sampleH`), nicht über die
  Render-Objekte; die Fläche steht ihm nicht im Weg. Das gilt dann auch für den Wasser-Pinsel
  (Schritt 7), wenn er denselben Weg nimmt, und es stimmt nur für die ERSTE Landschaft der Szene
  (`tvw.front()`).
- **Kosten**: `allBodyCells` und `bodyCells` laufen über das ganze Raster, einmal je Neuaufbau-Tick
  (65 536 Zellen bei der Standardauflösung, 16 Mio. bei 4096). Bei Pinselrate und hoher Auflösung
  gehört das Rechteck der letzten Änderung als Suchfenster hinein (Schritt 7).

## Was nicht gemacht ist

- **D3D11, D3D12, Vulkan**: weder Code angefasst noch gelaufen (Schritte 9 und 10, Windows-Hardware).
  Die Fläche ist ein gewöhnliches Mesh mit gewöhnlichem Material, es gibt keinen Sonderpfad, der
  dort nachzuziehen wäre; gezeigt ist das aber nur auf Metal und OpenGL. Offen für Schritt 9:
  Rückfälle im Transluzent-Pfad (Docs §3 des Wasser-Shader-Plans, z. B. Vulkan zeichnet Skinned
  nach den Transparenten), die jetzt das Wasser treffen.
- **Ufer-Beschneidung und Aushub** (Schritte 5 und 6). Die Fläche ist der Fußabdruck des Feldes auf
  Spiegelhöhe, sie steht über Gelände, das höher liegt. Der Ansatzpunkt für Schritt 5 ist das
  Eckengitter (`buildLattice`): "nass UND unter dem Spiegel" ist das Minimum zweier solcher Arrays; und
  Schritt 5 liest die Terrain-Dirty-Flags im selben `updateTerrains`, in dem die Wasserfläche jetzt nachgeführt wird.
- **Material wählbar**: alle Flächen nehmen das Engine-Wasser. Ein Feld `Field::material` (und
  Parameter-Overrides je Körper) wäre ein eigener Schritt; die Entities werden nie gespeichert, ein
  Eingriff am Inspector ginge bei jedem Undo verloren.
- **Verpackung**: das Spiel liefert den ganzen Engine-Ordner mit (Wetter-Töne laufen so), also auch
  `Engine/Materials/Water.hasset`; nicht in einer verpackten Szene ausprobiert.
- Kein Durchlauf mit echter Maus im laufenden Editor.

## Fallen

- Der Wasser-Hash vergleicht Gitterwerte, nicht Zellen: ein Pinselstrich, der die coverage eines
  fremden Körpers ändert, baut den benachbarten See nicht.
- Zwei Schnittpunkte können dieselben Koordinaten haben (an einer Ecke trifft jede Gitterkante
  denselben Punkt). Beim Vereinfachen darf eine Entscheidung nie gegen einen Punkt fallen, den dieselbe
  Runde schon weggeworfen hat: erst so ging die Ecke `(-9, -8)` des Rechtecks verloren (11 m²) und
  ein halbes Rand-Rechteck fehlte. Der Test prüft die acht Punkte einzeln.
- Zufallstest für Polygone mit Loch: Winkel müssen je Sektor gestreut werden, sonst liegt bei einer
  Lücke über 180° der Mittelpunkt außerhalb und das "Loch" auch.
- `WaterSurfaceComponent` taucht in `populateEveryComponent` nicht auf: sie wird nie gespeichert.
