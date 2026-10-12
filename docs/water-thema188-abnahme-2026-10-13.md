# Thema 188 Schritt 3: Abnahme Wasser-Pinsel-Grube und mitgehendes Seebett (13.10.2026)

Geprüft auf NN-WS03 (RTX 4070, laut `nvidia-smi` rendert jeder Lauf dort), Debug-Build des Zweigs
`claude/wasser-pinsel-lake-grube-fix` (Stand 6fa1a087 + Zeugen 2d894979), privater Deploy `C:/hw188/deploy`,
frisches `APPDATA` je Aufnahme. D3D12 mit Debug-Layer + DRED, Vulkan mit Validation-Layer (`HE_GPU_DEBUG=1`);
D3D11 hat keinen Debug-Layer, dort zählt nur Bild und Logzeile.

## Ergebnis

| Prüfung | D3D11 | D3D12 | Vulkan |
|---|---|---|---|
| Pinsel-Grube deckungsgleich (`HE_DUMP_WATERBRUSH=pit`) | ✔ | ✔ | ✔ |
| See: Bett folgt Lappen/Bucht (`HE_DUMP_LAKEEDIT=reshaped`) | ✔ | ✔ | ✔ |
| See: Bett folgt Frame für Frame (`=drag`) | ✔ | ✔ | ✔ |
| See: hin und zurück bit-genau (`=dragback`) | ✔ | ✔ | ✔ |
| See: Play-Modus, Collider, Stop (`=play`) | ✔ | ✔ | ✔ |
| `[ERROR]`-Zeilen im Log | 0 | 0 | 0 |

Die Logzahlen sind auf allen drei Backends identisch (die Geometrie entsteht auf der CPU). Die Bilder weichen von
Vulkan im Mittel um 1,0/255 (mit animiertem Wasser) bzw. 0,4/255 (Boden allein) ab.

Vollständige Testsuite (Debug, `he_tests.exe`, eigenes `APPDATA`): **5101 bestanden, 0 fehlgeschlagen**,
1 300 030 Assertions, 23 min. Ausgeschlossen, wie in den früheren Schritten: die drei Gamepad-Fälle
„End-to-end: …“ (echter Xbox-Controller am Rechner) und „PhysicsWorld: a full contact cache…“ (Jolt-Debug-Assert,
auch auf der Basis). CI 38311805813 auf 6fa1a087: grün auf Windows, Linux, Linux·Vulkan (lavapipe) und macOS.

## Wasser-Pinsel: Grube gegen Fläche

`HE_DUMP_WATERBRUSH=pit` stellt den Fall nach, in dem der trockene Krater auftrat: Gelände mit 8 % Steigung nach
Osten, das Pacing des Editors (Stärke 5 · 1/60 s, ein Dab je halben Meter, 97 Dabs), der Standardpinsel (Radius 10,
Falloff 5) und Dig Bed an. Das Landscape hat 128 m bei 129 Vertices und 32 Wasserzellen, also die Proportionen des
Editors. Gezählt wird gegen das Mesh, das `buildSurface` baut (das, was `WaterSurface` zeichnet), nicht gegen
`insideSheet`, denn sonst prüfte das Gate sich selbst.

| | abgesenkte Vertices | davon trocken | tiefste trockene Stelle | Grube x / z | Fläche x / z |
|---|---|---|---|---|---|
| Fix | 1337 | **0** | – | −36…24 / ±12 | −38,06…24,34 / ±12,43 |
| Negativkontrolle (Gate aus) | 1887 | **450** | 4,68 m | −38…32 / ±14 | −38,06…26,09 / ±12,53 |

Die Grube liegt jetzt ganz unter dem Wasser. Die Fläche reicht 0,3 bis 2 m darüber hinaus: Dort liegt flaches
Wasser auf ungegrabenem Boden, das ist der Pegelversatz von 0,3 m und keine Grube.

![Fix: die Grube endet an der Wasserlinie](water-lake/thema188-pit-after-vulkan.png)
![Negativkontrolle: trockener Krater bergauf neben dem Wasser](water-lake/thema188-pit-negctrl-vulkan.png)

**Bleibt sichtbar (kein Fehler im Sinne des Themas, zur Entscheidung):** Bergauf schneidet der Pinsel bis auf den
Wasserspiegel, und darüber steht eine senkrechte Böschung von bis zu 4 m (Gelände über dem Pegel, das abgetragen
wurde). Am Nordrand ist sie gezackt, weil das Vertexgitter (1 m) gegen die Uferlinie der 4-m-Wasserzellen läuft.
Trockener Grubenboden ist dort nicht zu sehen. Mit `HE_DUMP_WATERHIDESHEET=1` sieht man den Boden allein; auf dem
Bild ist dann nur die Wand zu sehen.

## Lake-Werkzeug: Das Bett geht mit

- **`=reshaped`:** Boden unter dem herausgezogenen Lappen (27, −12) geht von 0,00 auf −2,44 m, in der
  hineingedrückten Bucht (4, 16) von −0,65 auf 0,00 m. Mit der Negativkontrolle (`follow` → `reshape`) bleiben beide
  Werte unverändert: 0,00 → 0,00 und −0,65 → −0,65.
- **`=drag`:** Die Ostspitze wird in 12 Frames je 1 m von (30,0) auf (42,0) gezogen, mit einem `updateTerrains` je
  Frame wie im Editor. Der Boden bei (36,0) folgt in jedem Frame (0,00 … −0,18 / −0,63 / −1,17 / −1,62 / −2,01 /
  −2,25 m), die Chunks sind am Ende jedes Frames neu gebaut. Mit der Negativkontrolle bleiben alle 12 Frames bei
  0,00, und kein Vertex ändert sich.
- **`=dragback`:** Hin und zurück (24 Frames) ergibt **0 abweichende Vertices**, auch auf 1025².
- **`=play`:** `setPlayMode(true)`, Spitze auf (40,0), dann `SceneSystems::tickWorld` mit der Physikwelt.
  - Boden 0,00 → −1,62 m im selben Frame.
  - Der Collider (Raycast) steht im Zieh-Frame noch auf 0,00 und einen Frame später auf −1,62. Das ist gewollt:
    Er wird nachgezogen, sobald das Terrain einen Frame ruht (`TerrainSystem.cpp`).
  - Nach Stop (Snapshot-Restore) ist der Boden wieder 0,00, der See gräbt weiter, und der Patch ist erhalten.
  - Dasselbe Ziehen im Editmodus gräbt danach wieder (−1,62 m).
  - Mit der Negativkontrolle bleiben Boden und Collider bei 0,00.

![Fix: Bett (Wasser ausgeblendet) reicht bis zur gezogenen Spitze](water-lake/thema188-drag-bed-after-vulkan.png)
![Negativkontrolle: Bett bleibt auf der alten Form, die Spline zeigt schon die neue](water-lake/thema188-drag-bed-negctrl-vulkan.png)

**Kosten je Zieh-Frame** (Debug, `updateTerrains` gesamt, also Sync, Neu-Graben, Chunk-Neubau und Wasserfläche):

| Raster | Frame | ohne Neu-Graben (Negativkontrolle) |
|---|---|---|
| 129² auf 100 m (≈ Editor-Standard 1 m/Vertex) | 8,7 ms Mittel, 12,6 ms schlimmster | 2,7 ms |
| 1025² auf 100 m (10 cm/Vertex, 164 000 gegrabene Vertices) | 124 ms Mittel, 137 ms schlimmster | – |

Der zweite Fall ist ein Stresstest mit 64-facher Vertexdichte; Release ist nicht gemessen.

## Nicht geprüft / offen

- Kein echtes Ziehen mit der Maus im Editorfenster. Der Mensch sitzt an der Konsole; die Zeugen rufen dieselben
  Funktionen in derselben Reihenfolge wie der Frame des Editors (`SceneSystems::tickWorld` ruft
  `updateTerrains` in jedem Frame, im Edit- wie im Play-Modus, `EditorApplication.cpp`).
- Das In-Editor-Handbuch (`he-docs.json`, Website-Repo) beschreibt das Ziehen noch als „only reshapes the water“.
- Grenzen aus Schritt 2 gelten weiter:
  - Ein Auflösungswechsel verwirft den Patch, und das alte Bett bleibt als Loch.
  - Überlappende Seen addieren ihre Patches.

Zum Nachstellen: `HE_DUMP_WATERBRUSH=pit` bzw. `HE_DUMP_LAKEEDIT=drag|dragback|play` mit `HE_DUMP_SKYTEST=1
HE_DUMP_CAMY=375 HE_DUMP_CAMZ=48 HE_DUMP_PITCH=-60`, optional `HE_DUMP_WATERHIDESHEET=1` (Boden allein) und
`HE_DUMP_LAKERES=1025`.
