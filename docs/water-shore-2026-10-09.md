# Ufer-Beschneidung: Wasser steht nicht über das Gelände (Thema 174, Schritt 5)

Stand 09.10.2026, Mac, Debug-Build. Schritt 4 machte aus dem Wassermodell eine flache Fläche je
Körper. Diese Fläche war der Fußabdruck der Zellen auf Spiegelhöhe, auch dort, wo das Gelände
höher liegt. Jetzt endet sie dort, wo das Gelände durch das Wasser kommt. Das gilt für gebürstetes
Wasser und für Seen aus einem Polygon gleich, weil die Beschneidung im Mesh-Aufbau sitzt und nicht in
den Zellen: die Zellen bleiben die Absicht (was gemalt oder gezeichnet wurde), die Fläche ist, was davon
übrig bleibt. Ein Hügel, der später in einen See modelliert wird, nimmt ihm das Wasser vom Gipfel,
ohne das Feld anzufassen; wird er wieder abgetragen, ist das Wasser zurück.

Nicht angefasst, wie vorgegeben: das Gelände (nur gelesen, `terrainHeightAt`), Aushub (Schritt 6).

Code: `WaterMesh.h/.cpp` (Gitter und Konturen), `WaterField.h/.cpp` (Einstellungen,
`setShoreClip`, `noteGroundChanged`), `WaterSurface.cpp` (Parameter, Neuaufbau), `TerrainSystem.cpp`
(Übergabe der Geländeänderung), `SceneSerializer.cpp`, `InspectorPanel.cpp` + `EditorHelp.cpp`
(Terrain, Abschnitt Water), Witness `HE_DUMP_WATERLAKE=slope|bowl|brush` in `EditorApplication.cpp`.
Tests in `tests/test_water_surface.cpp` (12 neue Fälle) und `tests/test_water_field.cpp` (1).

## Ablauf in einem Absatz

Das Eckengitter aus Schritt 4 bekommt ein zweites Feld `g`: an jeder Ecke, die Wasser ist oder neben
einer Wasserecke liegt, die Tiefe des Wassers über dem Gelände, `Spiegel + Überstand − Geländehöhe` in
Metern (`terrainHeightAt`, nur gelesen). Marching Squares schneidet jetzt **beide**: eine Ecke ist
innen, wenn ihre coverage bei 127,5 liegt **und** `g ≥ 0`. Läuft eine Gitterkante von innen nach außen,
liegt der Schnittpunkt dort, wo die **erste** der beiden Grenzen erreicht wird (beim Eintritt die letzte),
jede linear interpoliert. Auf einem ebenen Hang liegt die Kontur damit auf Gleitkommagenauigkeit dort,
wo Wasser und Gelände sich treffen. Alles danach (Konturen schließen, Löcher zuordnen, Ear-Clipping)
weiß nichts vom Gelände: ein Ufer, eine Insel, die das Gelände aus dem See hebt, und ein Tümpel in einer
Senke sind Konturen, Außenränder und Löcher wie jede andere Form auch.

## Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Wo sitzt die Beschneidung | im Gitter und in den Konturen, nicht auf den Zellen und nicht als Nachbearbeitung des Meshes | Der Hinweis aus Schritt 4 stimmt: "nass UND unter dem Spiegel" ist das Minimum zweier Felder an den Ecken. Pinsel und Lake haben denselben Weg, ein Test belegt dieselbe Uferlinie für beide. |
| Schnitt zweier Felder | Schnittpunkt je Kante aus beiden Grenzen einzeln, nicht Interpolation über `min(coverage, g)` | `min` an den Ecken verschiebt die Schnittstelle, sobald die Ecke von einem der beiden Felder begrenzt wird, das der Kante nicht gehört. Einzeln gerechnet ist der Schnittpunkt im Sinne des Gitters exakt. Das alte Verhalten ohne Gelände bleibt bit-gleich (`g` leer = nur coverage). |
| `≥ 0`, nicht `> 0` | innen schließt Gleichheit ein | Der Standardfall (See auf Höhe 0 über flachem Gelände 0) wäre sonst überall leer. |
| Sattelzelle | die Mittel beider Felder müssen innen liegen | Wie der Schritt-4-Sattel, nur für beide Felder. |
| Wo wird das Gelände gelesen | nur an Wasserecken und ihren vier Nachbarn | Eine Kante braucht eine Innenecke. Ein See in einer großen Box kostet seine eigenen Ecken. Alles andere bekommt "sehr tief" (1e6) und kommt nie als Grenze vor. |
| Nicht endliche Geländehöhe | zählt als weit über dem Wasser | Kein NaN in den Vergleichen, deterministisch. |
| Einstellungen | **Feld der Landschaft**, nicht je Körper: `Field::clipToGround` (an) und `Field::shoreOvershoot` (0,05 m, 0 bis 10) | Schritt 5 liefert keine Körperliste im Inspector; eine Einstellung je Landschaft reicht für "an/aus und kleiner Überstand" und gilt für Pinsel und Lake gleich. Je Körper wäre ein Zusatz, wenn Schritt 7/8 das Werkzeug dafür bauen. |
| Überstand | **senkrecht** in Metern, Gelände bis `Spiegel + Überstand` zählt als unter Wasser | Ohne Rand endet die Fläche einen Haar vor der Böschung (das Terrain-Mesh ist zwischen den Eckpunkten und nach LOD-Verkleinerung nicht das Höhenfeld), eine helle Linie trennt Wasser und Ufer. Senkrecht ist die einzige Größe, die sich unabhängig von der Neigung erklären lässt; auf flachem Ufer schiebt sie den Rand weiter, auf steilem weniger. |
| Standard an | ja | "Wasser steht nicht über das Ufer hinaus" ist das Verhalten, das die Aufgabe will. `SurfaceOptions::clipToGround` der reinen Geometriefunktion ist dagegen aus; `WaterSurface` reicht die Einstellung des Feldes durch. |
| Neuaufbau bei Geländeänderung | `TerrainSystem::updateTerrains` übergibt das Rechteck, das es gleich löscht (oder die ganze Landschaft bei `dirty`/neuem Gitter), vor dem Löschen an `noteGroundChanged`, um eine Geländezelle verbreitert | Die Wasserfläche läuft erst danach und sähe `regionDirty` nicht mehr. Nur wenn Körper da sind und Beschneidung an ist. Die Richtung Wasser → Gelände bleibt aus (kein `dirty`, kein `regionDirty` durch Wasser). |
| Gleicher Hash | schließt `g` und die Einstellungen ein | Sonst wäre "Hügel unter dem See modelliert" ein gleicher Hash, und die Fläche würde als unverändert behalten. |
| Einstellung setzen | `setShoreClip(tc, an, überstand)` markiert alles dirty; der Inspector nimmt diesen Weg und **nicht** `changed` (das würde alle Chunks neu bauen) | Eine vollständig weggeschnittene Fläche hat keine Entity und fiele durch den Parameter-Vergleich: nur das Dirty-Flag erreicht sie. |
| Szenenformat | `waterClip` (nur wenn aus) und `waterShoreOvershoot` (nur wenn nicht 0,05), `pristine()` kennt beide | Eine Landschaft ohne Wasser speichert byte-gleich wie vorher; ältere Szenen mit Wasser laden als "beschnitten". `sanitize` klemmt einen von Hand editierten Wert. |

## Wann wird was neu gebaut

Wie in Schritt 4, dazu: ein Geländeeingriff (Sculpt, Generate, Heightmap-Import, Größe/Auflösung der
Landschaft) markiert über `noteGroundChanged` das betroffene Rechteck im Wasserfeld. Körper, die es
treffen, bauen ihr **Gitter** neu (Geländeabtastung und Hash); das Mesh wird nur ersetzt, wenn das
Gitter sich geändert hat. Weil `g` das ganze Gelände unter dem See enthält, baut ein Sculpt mitten im
See seine Fläche bei jedem Pinselframe neu, auch wenn die Kontur gleich bleibt (Hash über rohe Höhen).
Das ist ein Gitterbau und ein Mesh-Austausch eines Sees, deutlich weniger als das, was derselbe Strich
an den Terrain-Chunks tut; entprellt ist es nicht. Ein Strich weit weg von jedem See baut nichts
(`kept`), und mit Beschneidung aus löst kein Geländeeingriff etwas aus.

## Belege

Tests: `he_tests -tc='Water*'`, 73 Fälle (60 aus Schritt 3/4 unverändert grün, 13 neu). Dazu
`ctest -R "scene|terrain|water|help|mcp_tools|inspector|undo|foliage|navigation|replic|outliner|viewport|spline"`,
52 von 52 grün, und `editor_help_audit` (zwei neue Hilfe-Einträge `Terrain/Clip To Ground`, `Terrain/Shore Overshoot`).

| Fall | Erwartung | Ergebnis |
|---|---|---|
| Gitter von Hand mit Geländetiefe (Rampe, eine Ecke je Meter) | Kontur bei 1,5 (coverage) und 3,75 (Gelände), Fläche 8,375 | exakt |
| See über Hang (0,5 m je m), Spiegel 3 m | Ufer bei `x = (3 + 0,05) / 0,5 = 6,1`, Fläche `26,1 · 40 − 1` | auf 1e-5 genau; Fußabdruck allein 1600 m², Gelände bis 5 m über dem Spiegel |
| Überstand 0 / 0,05 / 0,25 / 1 | Ufer bei 6,0 / 6,1 / 6,5 / 8,0 | auf 1e-5 genau |
| Mulde (Paraboloid), Spiegel 4 | Kreis mit `r = √(4,05 / 0,01) = 20,125`, jeder Eckpunkt auf dem Kreis | Fläche auf 1 %, Radius je Punkt auf 0,5 % |
| Hügel im See | Insel als Loch (2 Konturen, 1 Polygon), Fläche `2500 − π · 165` | auf 1 % |
| Zwei Mulden, ein Körper | zwei Außenränder, Rücken dazwischen trocken | auf 1 % |
| Pinsel (`addCircle`) auf demselben Hang | dieselbe Uferlinie wie der Polygon-See | 6,1, auf 1e-5 |
| Gelände unter dem Spiegel überall | Mesh **bit-gleich** mit und ohne Beschneidung | ja |
| Gelände über dem Spiegel überall | keine Fläche, Fußabdruck allein weiter da | ja |
| Orakel für jede Form | am Eckpunkt und im Schwerpunkt jedes Dreiecks `Gelände − Spiegel ≤ Überstand` | ja; bei der Insel `+ 0,02` (siehe Grenzen) |
| Welt: Hügel per `TerrainSculpt::apply` in den See | Fläche folgt (`rebuilt == 1`), Gipfel trocken, Zellen unverändert; Hügel abgetragen: Wasser zurück | ja |
| Welt: Strich weit weg / Beschneidung aus | `kept == 1` bzw. nichts gebaut | ja |
| Welt: Fläche ganz weggeschnitten (Spiegel −5 über Gelände 0) | keine Entity, Wasser im Feld bleibt; Beschneidung aus: Entity da; an: wieder weg; Spiegel angehoben: wieder da | ja |
| Einstellungen: klemmen, NaN behält den alten Wert, `sanitize` | | ja |
| Szenenformat | Standard schreibt keinen Schlüssel; Rundlauf als Datei und als Undo-Snapshot; handgemachte Werte | ja |

Bilder (`docs/water-surface/`), Debug-Editor, Himmel `HE_SKY_TIME=1.0`, `TOD=0.4`, Forward-Pfad, AA aus,
Kamera `CAMY=375 CAMZ=48 PITCH=-60`, Spiegel 2 m. Das Gelände steigt nach Osten (0,12 m je m) mit
einer Welle quer dazu, der See-Polygon läuft über den Hügel hinaus. `HE_DUMP_WATERLAKE=slope|bowl|brush`,
`HE_DUMP_WATERCLIP=0|1`, `HE_DUMP_WATERHIDEGROUND=1` (Landschaft nicht zeichnen, nur das Wasserblatt).

| Bild | Metal | OpenGL |
|---|---|---|
| Hang, beschnitten | `metal-slope-clip-on.png` | `opengl-slope-clip-on.png` |
| nur Wasserblatt, beschnitten | `metal-slope-sheets-clip-on.png` | `opengl-slope-sheets-clip-on.png` |
| nur Wasserblatt, nicht beschnitten | `metal-slope-sheets-clip-off.png` | `opengl-slope-sheets-clip-off.png` |
| Mulde, beschnitten | `metal-bowl-clip-on.png` | `opengl-bowl-clip-on.png` |
| nur Wasserblatt der Mulde, nicht beschnitten | `metal-bowl-sheets-clip-off.png` | |
| Pinsel am Hang, beschnitten | `metal-brush-clip-on.png` | |
| Unterschied beschnitten/nicht beschnitten, rot markiert | `metal-slope-diff-on-off.png` | |

Der Log des Dumps ist der numerische Zwilling (`ground over sheet at the vertices at most`):

| Fall | Dreiecke | x-Spanne der Fläche | höchstes Gelände über dem Spiegel an den Eckpunkten |
|---|---|---|---|
| Hang, beschnitten | 325 | −59,99 … 27,06 | 0,050 m (der Überstand) |
| Hang, nicht beschnitten | 421 | −59,99 … 59,99 | 6,289 m |
| Mulde, beschnitten | 354 | −44,86 … 44,86 | 0,050 m |
| Mulde, nicht beschnitten | 404 | −53,97 … 57,96 | 14,355 m |
| Pinsel, beschnitten | 599 | −54,00 … 27,06 | 0,050 m |

Die Mulde läuft bei Spiegel 2 m und `0,004 · r² − 6` bis `r = √(8,05 / 0,004) = 44,86`: dieselbe Zahl wie die Spanne.
Metal und OpenGL zeigen dieselbe Uferlinie.

**Was die Bilder zeigen und was nicht.** Von oben sind beschnittene und nicht beschnittene Fläche im
Normalbild **nicht** zu unterscheiden, und das ist richtig: der Teil des Blatts, den die Beschneidung
entfernt, liegt unter dem Gelände, und das Terrain ist undurchsichtig. In den beiden Metal-Läufen
unterscheiden sich 2881 Pixel (0,3 %), über das ganze Wasser verstreut (`metal-slope-diff-on-off.png`),
keiner am Ufer gebündelt: das ist die andere Triangulierung (325 gegen 421 Dreiecke) in den
Glanzpunkten der Wasseroberfläche, der Rauschboden zweier gleicher Läufe ist byte-gleich. Sichtbar wird
die Beschneidung dort, wo das Gelände nicht deckt: mit unsichtbarem Gelände (Blatt allein), unter der
Landschaft, und in allem, was die Fläche liest statt zeichnet (Dreieckszahl, Kosten, künftig Schwimmen).

## Grenzen (bekannt, nicht in diesem Schritt)

- **Das Ufer ist so fein wie das Wassergitter.** Zwischen zwei Ecken ist die Kontur eine Sehne. Bei
  `Field::res = 256` auf einer 1000-m-Landschaft ist eine Zelle 3,9 m; auf gekrümmtem Ufer liegt die Kontur
  bis zu einer Zelle neben der wahren Linie (der Überstand fängt Unterschreitungen ab). Wer ein feines
  Ufer will, hebt `water.res` an (Inspector-Zeile dafür fehlt weiter). Eine automatische Verfeinerung
  des Gitters nur für das Gelände wurde nicht gebaut: sie würde die Kontur der coverage ändern und die
  Schritt-4-Tests (acht Eckpunkte des Rechtecks) neu schreiben.
- **Inseln liegen unter der Sehne.** Ein Loch ist ein Polygon, dessen Ecken auf dem Ufer liegen; zwischen
  zwei Ecken ragt ein Streifen Wasserblatt um Sehnenpfeil mal Neigung unter den Inselrand (ein bis zwei
  Zentimeter beim Test). Unsichtbar unter dem undurchsichtigen Gelände; der Test lässt dort `+ 0,02` zu.
- **Tessellierung und Displacement** sind nicht im Höhenfeld, das `terrainHeightAt` liest; wer starke
  Displacement-Werte setzt, zeichnet das Gelände bis zur halben Stärke neben dem Wasserrand. Der Überstand
  ist dafür gedacht.
- **Fragen an das Wasser** (`levelAt`, `bodyAt`, `coverageAt`) kennen das Gelände nicht: ein Punkt auf der
  Böschung im Polygon ist für sie nass, auch wenn dort keine Fläche steht. Schwimmen und Auftrieb sind nicht
  Teil des Themas; wer sie baut, braucht die Beschneidung auch in diesen Abfragen.
- **D3D11, D3D12, Vulkan** weder angefasst noch gelaufen; die Beschneidung ändert nur das Mesh
  (Schritte 9 und 10 prüfen den Weg auf Windows-Hardware).
- **Kein Durchlauf mit echter Maus**: der Inspector-Abschnitt ist nur gebaut und im Hilfe-Audit
  gezählt, nicht im laufenden Editor bedient worden (der Editor-MCP war nicht verbunden).

## Fallen

- `shapeGround` im Test: das Höhenfeld muss `resolution × resolution` groß und die Auflösung `2ⁿ + 1` sein,
  sonst fällt `terrainHeightAt` auf Rauschen/0 zurück und `TerrainSystem` tastet das Feld neu ab.
- `TerrainSystem` löscht `regionDirty` und das Rechteck **vor** `WaterSurface::update`: die Übergabe musste in
  die Chunk-Schleife, davor.
- Der Schnittpunkt zweier Grenzen darf nicht aus `min(coverage, g)` an den Ecken interpoliert werden; er
  gehört je Grenze einzeln gerechnet und dann nach Richtung (Austritt: erste, Eintritt: letzte).
- Die ersten Edits bei Landschaft `dirty = true` (jede neue Landschaft, jeder Rig im Test) markieren das
  ganze Wasserfeld dirty: harmlos, die Flächen werden dann ohnehin zum ersten Mal gebaut.
- Der Witness zeichnet im Normalbild von oben den Unterschied nicht (Gelände deckt): für den A/B
  `HE_DUMP_WATERHIDEGROUND=1`.
