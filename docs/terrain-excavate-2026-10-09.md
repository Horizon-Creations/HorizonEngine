# Terrain-Aushub unter einer Spline-Form (Thema 174, Schritt 6)

Stand 09.10.2026, Mac, Debug-Build. Das Lake-Werkzeug (Schritt 8) braucht unter
dem Wasser ein Becken. Dieser Schritt liefert die Funktion dafür im
Landscape-Code; Wasser und Werkzeuge sind bewusst nicht angefasst.

Code: `TerrainSculpt::excavatePolygon` (`TerrainSculpt.h`, ausführlich
kommentiert), `src/HE_Scene/src/TerrainSculpt.cpp`. Tests:
`tests/test_terrain_excavate.cpp` (20 Fälle).

## Was sie tut

Ein Polygon in terrain-lokalem XZ (`glm::vec2`, x → x, z → y, ohne doppelten
Schlusspunkt, gerade-ungerade gefüllt): genau das Objekt, das
`water::polygonFromPolyline` liefert. Das Lake-Werkzeug gibt also dasselbe Polygon
an `water::addPolygon` und an den Aushub.

| Modus | `amount` | Wirkung innerhalb des Polygons |
|---|---|---|
| `Floor` | Höhe des Beckenbodens (terrain-lokales Y) | Boden über dem Floor wird auf ihn gesenkt; Boden, der schon tiefer liegt, bleibt (ein Aushub hebt nie). |
| `Dig` | Tiefe in Metern | Jeder Vertex sinkt um die Tiefe, die Form des Geländes bleibt. |

`falloff` ist die Breite der Böschung, **von der Umrisslinie nach außen**: auf und
in der Linie ist das Gewicht 1, nach `falloff` Metern 0, dazwischen Smoothstep
(ohne Knick am Außenrand; der lineare Pinsel-Verlauf ließe bei einer so breiten
Grube eine sichtbare Kante). 0 = harte Kante. Außerhalb der Böschung wird kein
Bit geschrieben.

Gerechnet wird je Terrain-Vertex. Innen bekommt ein Vertex exakt den Floor (zugewiesen,
nicht gemischt: `h + 1·(floor − h)` ist nicht bit-gleich), außerhalb der Böschung
bleibt er bit-gleich. Ein Vertex auf der Linie zählt als innen, bei schrägen Kanten
bis auf Rundung.

Seit Thema 174 Schritt 13 gibt es (nur `Floor`) zusätzlich `bankIn`/`edgeHeight`: eine
zweite Böschung, **nach innen** von der Linie gemessen. Innerhalb bleibt es wie oben
(der Floor, ab `bankIn` Metern von der Linie); dazwischen blendet die Höhe smoothstep
von `edgeHeight` genau auf der Linie zum Floor. `bankIn = 0` (Default) ist bit-gleich
zum alten Verhalten — nur `WaterLake.cpp` setzt es, siehe `terrain-excavate`-Hinweis
unten und `docs/water-lake-2026-10-10.md`.

Aufwand: Innen/Außen per Scanline je Gitterzeile, der Abstand nur kantenweise in
der Böschung. Ein See von 600 m Durchmesser mit 256 Punkten und 20 m Böschung auf
1025² Vertices braucht im Debug-Build rund 90 ms.

## Chunks und LODs

Wie `TerrainSculpt::apply`: `ensureHeights` zuerst, nur `sculptHeights` wird
geschrieben, `regionDirty` und das Rechteck werden gesetzt (die Vertices, die sich
bewegt haben, plus ein Gitterschritt, damit beide Chunks eines Randvertex neu
gebaut werden). `dirty` bleibt unberührt. `TerrainSystem` baut daraufhin jeden
Chunk unter dem Rechteck mit **allen** LOD-Stufen neu (gleiche Mesh-UUIDs, der
Renderer lädt nur neu hoch) und reicht dasselbe Rechteck an die Ufer-Beschneidung
weiter (`water::noteGroundChanged`). Der Test belegt beides: nur die Chunks unter
der Grube ändern sich, auf allen vier Stufen, auch über eine Chunk-Grenze hinweg,
und ein Wasserkörper, den ein Plateau verdeckt hatte, bekommt seine Fläche, sobald
darunter ein Becken ist.

## Undo

Der Undo-Eintrag ist der des Editors: **eine** `EditorUndo::snapshotNow` vor
**einem** Aufruf. `sculptHeights` wird als rohe Float-Bytes gespeichert, der
Snapshot gibt den alten Boden also bit-genau zurück (der Test vergleicht per
`memcmp`). Das gilt auch für den Erstkontakt: war die Landschaft nie
modelliert, backt `ensureHeights` das Rauschen in `sculptHeights` und rastet
die Auflösung auf 2ⁿ+1; Undo stellt `sculptHeights = leer` und die alte Auflösung
wieder her. Ein eigener Patch-Undo in der Scene-Bibliothek wäre eine zweite
Wahrheit neben dem Editor-Snapshot, die niemand auslöst; er ist nicht gebaut.

## Für Schritt 8 (Lake-Werkzeug)

* Reihenfolge je Zug: Undo-Schritt, `excavatePolygon`, `water::addPolygon` mit
  demselben Polygon.
* Der Aushub merkt sich nichts und ist nicht umkehrbar: formt der Nutzer die Spline
  um, bleibt die alte Grube liegen, ein zweiter aufs neue Polygon vertieft nur
  (`Floor` hebt nie). Das ist eine Entscheidung für Schritt 8, nicht hier.
* `Floor` mit `level − depth` ist der naheliegende Aufruf für einen See; `Dig` gräbt
  um eine feste Tiefe unter das vorhandene Gelände.
* Das Polygon ist terrain-lokal, die Spline hat ein eigenes Entity: erst mit
  `polygonFromPolyline(polyline, toTerrainLocal)` umrechnen, die Matrix aus
  `HE::worldMatrixOf` beider Entities (nicht aus `TransformComponent::worldMatrix`,
  die ist einen Frame alt).
* Mit `falloff` > 0 sinkt die Böschung außerhalb des Umrisses unter den Spiegel, ohne
  dass dort Wasser liegt (die Fläche rastert nur das Polygon) — ein trockener, zu
  tiefer Ring, den die Beschneidung aus Schritt 5 nicht zudeckt. Für einen See ist das
  falsch (Thema 174, Schritt 13, Bugfix): `WaterLake.cpp` benutzt seitdem `bankIn` +
  `edgeHeight` (unten in `TerrainSculpt.h`) statt `falloff` — die Böschung liegt dann
  innerhalb des Umrisses, blendet von dort zum Spiegel, und das Wasser (unverändert
  auf dem Polygon gerastert) erreicht die Spline wirklich.

## Falle, die der Test gefangen hat

Die Scanline-Spanne wurde zuerst auf das Gitter geklemmt und dann benutzt: eine Spanne
ganz jenseits des Terrainrands landete dadurch auf der Randspalte und machte einen
Vertex außerhalb des Polygons zum Innenpunkt. Geklemmt wird jetzt erst, nachdem
Spannen außerhalb des Vertex-Rechtecks ausgeschieden sind. Gefunden hat es der
Vergleich gegen eine plumpe Referenz (Kreuzungszahl, Abstand zu jeder Kante, in
double) mit zufälligen Sternpolygonen, die oft halb auf dem Terrain liegen.
