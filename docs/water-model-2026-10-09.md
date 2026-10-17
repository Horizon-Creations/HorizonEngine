# Gemeinsames Wassermodell (Thema 174, Schritt 3)

Stand 09.10.2026, Mac, Debug-Build. Schritt 1 lieferte die Spline-Kurve, Schritt 2
das Spline-Werkzeug. Dieser Schritt legt die Datengrundlage fest, auf der der
Wasser-Pinsel (Schritt 7) und das Lake-Werkzeug (Schritt 8) arbeiten und aus der
die Wasserfläche (Schritte 4 und 5) gebaut wird. Mesh-Erzeugung und Editor-UI sind
bewusst nicht angefasst.

Code: `src/HE_Scene/include/HorizonScene/WaterField.h` (Vertrag, ausführlich
kommentiert), `src/HE_Scene/src/WaterField.cpp`, Mitglied
`TerrainComponent::water`, Speichern und Laden im `"terrain"`-Block von
`SceneSerializer.cpp`, Tests in `tests/test_water_field.cpp`.

## Das Modell in einem Absatz

Ein Raster über den 0..1-UV-Bereich der Landschaft (wie Weightmap und
Foliage-Maske) mit zwei Werten je Zelle: **coverage** (0..255, wie viel der Zelle
Wasser ist, am Ufer dazwischen) und **owner** (Id des Körpers, 0 = keiner). Dazu
eine kurze Liste von **Körpern** (`Body`): je einer ist eine Wasserfläche mit
einem Spiegel (`level`, terrain-lokales Y wie bei `terrainHeightAt`) und, bei
einem See, dem Entity-UUID seiner geschlossenen Spline (`sourceSpline`). Ein
Körper ohne Quelle ist Pinselwasser.

Das Feld liegt in der `TerrainComponent`, nicht in einer eigenen Komponente: es
gehört zu genau einer Landschaft, wird mit ihr kopiert, gespeichert und im
Undo-Snapshot mitgenommen, und es braucht keine neue Registrierstelle (kein
X-Makro, kein `isKnownComponentKey`, kein Hilfe-Scope).

## Entscheidungen, die die Schritte 4 bis 8 erben

| Frage | Entscheidung | Grund |
|---|---|---|
| Spiegel je Zelle oder je Körper | je Körper | Ein See ist eben. Zwei Körper mit verschiedenem Spiegel stehen einfach nebeneinander. |
| Auflösung | eigenes `water.res`, Standard 256, 1..4096 | Das Höhenfeld rastet auf 2ⁿ+1 und resampelt sich selbst (`TerrainSculpt::ensureHeights`); Wasser darf sich dabei nicht bewegen. Das Raster liegt im UV-Raum, also tut es das nicht. Wer Spiegel gegen Boden vergleicht, fragt `terrainHeightAt` an der Zelle. |
| Ufer | coverage statt 0/1 | Eine Polygonkante schreibt den echten Flächenanteil der Zelle, der Pinsel seinen Falloff. Wer ein Ja/Nein braucht, schneidet bei `kWet` (128); wer ein glattes Ufer will, interpoliert die Kontur zwischen den Zellen. |
| Körper-Ids | stabil (uint16), nie umnummeriert, nicht sofort wiederverwendet | Das Lake-Werkzeug formt um mit `clearBody` + `addPolygon` unter derselben Id; ein Pinselstrich kann seine Id über Undo halten. |
| Pinsel vs. belegte Zelle | der Pinsel erweitert, er nimmt nichts weg | `addCircle` übernimmt nur trockene Zellen; in einer fremden Zelle wird die coverage größer, der Besitzer bleibt. |
| Polygon vs. belegte Zelle | das Polygon ist maßgeblich ab `kWet` | Ein See, der über Pinselwasser gezeichnet wird, nimmt die Zellen, die er wirklich bedeckt; eine Randzelle, die er nur streift, bleibt beim Besitzer. |
| Umformen eines Sees | Zellen des Körpers löschen, neu rastern | Pinselkorrekturen am See gehen dabei verloren. Ein Overlay für "von Hand geändert" wäre eine eigene Entscheidung des Schritts 8. |
| Füllregel für Polygone | gerade-ungerade (even-odd) | Windungsrichtung egal, konkave Formen gehen, eine sich kreuzende Linie lässt die Überlappung trocken (Achter: beide Schleifen nass). |
| Dirty-Flag | `water.dirty` + Rechteck in terrain-lokalem XZ + `revision`; `TerrainComponent::dirty`/`regionDirty` bleiben aus | Eine Wasseränderung darf keine Terrain-Chunks neu bauen. Nach dem Laden ist alles dirty (die Fläche wird nie gespeichert). |

## API (Kurzfassung, Details in `WaterField.h`)

Alles in `HE::water`, alles terrain-lokal.

- **Körper:** `Field::createBody(level, sourceSpline)`, `findBody`, `findBySource`,
  `removeBody`, `clearBody`, `setLevel`, `pruneEmptyBodies`.
- **Kreis mit Falloff:** `addCircle(tc, body, x, z, radius, falloff, strength)`,
  `removeCircle(..., onlyBody)`. Gewicht wie `TerrainPaint`: 1 im Radius, linear auf
  0 über den Falloff, gemessen zum Zellmittelpunkt. Hinzufügen zieht die coverage
  Richtung 255, Entfernen zieht `round(255·a)` ab: derselbe Kreis mit derselben
  Stärke macht ein Hinzufügen auf trockenem Grund Byte für Byte rückgängig.
- **Polygon:** `rasterizePolygon` (reine Funktion, 8 Teilzeilen je Zelle,
  exakt quer dazu), `addPolygon(tc, body, poly)`, `removePolygon(tc, poly, onlyBody)`.
  Das Polygon hat **keinen** doppelten Schlussvertex: `Curve::polyline` einer
  geschlossenen Spline endet dort, wo sie begann, `polygonFromPolyline(ring,
  toTerrainLocal)` wirft den letzten Punkt weg und projiziert auf XZ. Die
  Transformation Spline-lokal → Welt → Terrain-lokal ist Sache des Aufrufers
  (Samples transformieren, nicht Kontrollpunkte).
- **Lesen:** `coverageAt` (bilinear), `bodyAt`, `levelAt`.
- **Chunk-Bezug:** `cellRect(tc, minX, minZ, maxX, maxZ)`,
  `chunkCellRect(tc, chunksPerSide, cx, cz)` (Zelle gehört dem Chunk, in dem ihr
  Mittelpunkt liegt; die Rechtecke zerlegen das Raster lückenlos), `anyWater`.
- **Auflösung:** `ensureGrid`, `setResolution` (nächste Zelle), `clearAll`.
- **Undo:** siehe unten. **Speichern:** `encodeCells`/`decodeCells`/`sanitize`.

## Speichern und Laden

Im bestehenden `"terrain"`-Block, nur wenn es etwas zu speichern gibt
(`!pristine()`), sonst ist die Szene Byte für Byte wie vorher:

```
waterRes, waterNextId,
waterBodies: [ { id, level, sourceSpline? } ],
waterCellsB64: Lauflängen (varint Anzahl, owner u16 LE, coverage u8), base64
```

Lauflängen statt zweier roher Raster: ein See in einem sonst trockenen Raster sind
einige Hundert Byte statt gut 190 KB je Undo-Snapshot. Der Lader prüft alles: ein
abgeschnittener oder überlaufender Strom wirft die Zellen weg und behält die
Körper (ein See kann aus seiner Spline neu gerastert werden), Zellen eines
unbekannten Körpers werden trocken, Körper mit Id 0, doppelter Id oder
nicht-endlichem Spiegel fallen weg, `waterRes` wird auf 1..4096 geklemmt, der
Id-Zähler wird hinter jede Id gesetzt. CBOR (Undo-Snapshot) läuft über dasselbe
JSON.

## Undo

Der Editor-Undo ist ein Snapshot der ganzen Welt (`EditorUndo`), und das Wasser
reist durch den Serializer mit: Strg+Z braucht aus diesem Abschnitt nichts.
`SceneSerializer`-Rundreise vor/nach einem Pinselstrich ist im Test belegt
("undo, which is a world snapshot, restores the water exactly").

Zusätzlich gibt es `Delta` (`makeDelta(before, after)`, `applyDelta`,
`revertDelta`): nur die Zellen, die sich bewegt haben, mit beiden Werten, plus
Körperliste vor/nach. Für einen Aufrufer, der "was hat dieser Strich geändert"
wissen will, ohne die Welt zu halten (Strichvorschau, MCP-Antwort, Zusammenführen
von Strichen). Der Editor-Undo braucht es nicht. Bei verschiedener Auflösung gibt
`makeDelta` false zurück: Resampeln ist kein Strich.

## Was Schritt 4 bis 8 daraus machen

- **Schritt 4/5 (Fläche, Ufer):** je Chunk `chunkCellRect` + `anyWater` zum
  Überspringen, `water.dirty`/Rechteck zum Neubauen, Kontur bei coverage 128 durch
  die Zellen (Marching Squares), Wasserspiegel `Body::level`, Ufer gegen
  `terrainHeightAt` beschneiden. `water.dirty` setzt die Fläche selbst zurück.
- **Schritt 6/8 (Aushub, Lake):** Spline-Entity → `Curve::polyline(tol)` →
  `polygonFromPolyline` mit der Welt→Terrain-Matrix → `createBody(level, uuid)` +
  `addPolygon`; Umformen = `clearBody` + `addPolygon`.
- **Schritt 7 (Pinsel):** pro Strich einen Körper anlegen (oder den unter dem
  Startpunkt fortsetzen: `bodyAt`), `addCircle`/`removeCircle`, am Strichende ein
  `snapshotNow`. Ein Werkzeug hält nur Körper-Ids und UUIDs über Frames, nie
  Entity-Handles (Undo baut alle Entities neu).

## Offene Punkte / Fallen

- `sourceSpline` ist eine Entity-UUID. Prefab-Instanziierung (Kopieren/Ctrl+D einer
  Landschaft samt Spline) vergibt neue UUIDs, der Verweis zeigt danach ins Leere;
  wer duplizieren darf, muss neu verknüpfen. Ein toter Verweis ist harmlos: die
  Zellen bleiben, nur das Umformen findet die Spline nicht.
- `addCircle` bleibt bei sehr kleiner Stärke wenige Prozent unter 255 stehen
  (Rundung), das liegt weit über `kWet`.
- Wer `TerrainComponent` neu zuweist (`tc = TerrainComponent{...}`), verliert das
  Wasser; die Editor-Wege dafür (Generate, Import Heightmap) ändern nur Höhen.
- Seit Schritt 5 im Feld: `clipToGround` (an) und `shoreOvershoot` (0,05 m), siehe
  `water-shore-2026-10-09.md`. Sie sind Einstellungen der Landschaft, nicht der Zellen; im Szenenformat
  nur, wenn sie vom Standard abweichen (`waterClip`, `waterShoreOvershoot`).
- Nicht gebaut (Stand Schritt 3): Fläche, Mesh, Ufer-Beschneidung, Aushub, Editor-UI, Inspector-Zeile
  für `water.res`.
