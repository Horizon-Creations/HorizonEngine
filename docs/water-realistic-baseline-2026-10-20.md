# Realistischeres Wasser: Bestandsaufnahme und Baseline (Thema 206, Schritt 1)

Stand 20.10.2026, Mac M5, Release-Editor `d895064e` + Witness-Haken; nur Screenshot-Werkzeug geändert.
Basis: `docs/water-shader-plan.md` §11/§12, `docs/water-shore-2026-10-09.md`.

## Handoff

### Ursache des zackigen Ufers (gemessen)
Die sichtbare Uferlinie ist **der Schnitt des Gelände-Meshes mit dem flachen Wasserblatt**, nicht die
Kontur des Wassergitters. Das Blatt hat überall Deckkraft ≈0,5 (`MatGen/main.cpp:670`, Blend Translucent
`:914`), keine Tiefe, keinen Randverlauf: die Kante ist hart, und auf flachem Strand verwandelt die kleinste
Geländefacette die Linie in einen Zickzack (Neigung 0,04: 3 cm Höhenrauschen = ±0,75 m seitlich).
- `beach` (Neigung 0,04, 3 cm Jitter am 1-m-Höhenfeld, Wasser 0,3 m über Grund) zeigt den Zickzack auf Metal
  **und** GL. `beach-smooth` (gleiche Szene ohne Jitter) hat eine glatte Kurve.
- `water.res` 256 gegen 33 (3,9 m Zellen = 1 km-Landschaft bei 256) gegen 1024 (`beach` / `rough*`, Metal):
  Dreiecke 607 / 80 / 2343, Bild praktisch gleich (0,01 bis 0,03 mittlere Abweichung, 0,6 bis 0,8 % der
  Pixel, nur Glanzpunkte). Die Sehnen-Vermutung aus "Grenzen" (`water-shore-2026-10-09.md:125`) trägt hier
  nicht: bei Mulde (R = 45 m) liegt die Sehnenabweichung bei 3,9 m Zellen unter 5 cm (`shore` gegen
  `shore33`: 0,15 % der Pixel). Eine Zelle bleibt für kleine, stark gekrümmte Seen relevant (nicht gemessen).
- Warum die Gitterkontur nicht zählt: `WaterMesh.cpp:259` klemmt das Blatt auf `Spiegel + 0,05 m`; was
  höher liegt, steckt unter dem Terrain und ist verdeckt. Sichtbar bleibt der Schnitt Ebene/Terrain.
  `paramsKeyOf` (`WaterSurface.cpp:55`) enthält `water.res`: eine Änderung baut das Mesh neu (die Tests
  oben belegen es: Dreieckszahl ändert sich).
- **Nicht ausgeschlossen:** Terrain-LOD in einer echten 1-km-Szene (Witness: 1-m-Höhenfeld, LOD0). Ein
  grobes LOD-Dreieck verstärkt dieselbe Ursache (größere Facetten). Wer die Szene des Menschen hat: `beach`
  gegen sie halten. Das Thema nennt keine Szene (Topic-Text leer, nur 4 Bilder).

### Referenzbilder (alle vier gelesen; `.avif` per `sips -s format png`)
- `28` Steilküste Rügen: glattes, dunkles Türkis, schmaler Flachwasserstreifen am Ufer, Horizont scharf (Farbverlauf flach→tief).
- `29` Meer von oben: feine, kurze, unregelmäßige Wellen, kaum Schaum, türkis→dunkelblau, Glanz nur auf Flanken (Ziel **Normalen/Chop**).
- `30` Strand: weiche Wasserkante, Schaumsaum mit Lochmuster, Sand durch das Wasser sichtbar, dann Grün-Blau (Ziel **Ufer** und **Transparenz**).
- `31` klares Flachwasser über Fels: Grund voll sichtbar, Kaustik auf dem Grund, Farbe aus Grund + Türkis (Ziel **Transparenz**).

### Baseline (vorher), `docs/water-realistic-baseline/baseline-<metal|opengl>-<view>.png`, 1280x720
Rezept: `python3 scripts/water_baseline_shots.py OUTDIR [--prefix after] --repeat 2` (alles gepinnt:
`HE_SKY_TIME=1.0`, `TOD=0.4 RENDERPATH=0 AA=0`, privates `HE_CONFIG_DIR`, `HE_HIDDEN_WINDOW=1`).
Ansichten: `open`, `open-hi` (Plane `WATERTEST=floor`, Kamera wie Plan §9.1 bzw. 14 m hoch), `shore`
/`shore33`/`shoresheet33` (Mulde `WATERLAKE=bowl`, Wassergitter 256 / 33 / nur Blatt),
`beach`/`beach33`/`beach-smooth`/`beach-sheet` (`WATERLAKE=slope WATERSLOPE=0.04 WATERLEVEL=0.3`,
`WATERROUGH=0.03`; Kamera `CAMX=8 CAMY=318 CAMZ=34 PITCH=-45`). Nach den Schritten 2 bis 4 mit
`--prefix after` wiederholen, mit `scripts/png_diff.py a.png b.png` vergleichen.
- Reproduzierbarkeit: zwei Läufe je Ansicht. `open`, `open-hi`, `*-sheet*`, `beach-smooth` Metal: md5-gleich.
  Ansichten mit gezeichnetem Gelände: 1 bis 60 Pixel (≤0,008 %) weichen um <1/255 mittel ab, Ursache nicht
  untersucht (Glanzpunkte/Geländeschatten). Als Rauschboden 0,01 % der Pixel ansetzen, nicht md5.
- Wasser wirkt in `shore`/`beach` fast deckend dunkelblau, ohne Tiefenabhängigkeit; Zickzack auf Metal und GL.

### Plan (Aufteilung Schritt 2 bis 4 bestätigt, mit einer Gabelung für 2/4)
Die Gabelung entscheidet die Queen: Schritt 2 (Rand) und 4 (Transparenz) brauchen beide **Wassertiefe je Pixel**.
A. **Tiefe ins Mesh backen** (empfohlen, Metal/GL zuerst, ohne Renderer): `WaterMesh.cpp` hat `g = Spiegel +
   Überstand − Grund` an den Gitterecken. Als Vertexfarbe/UV2 ausgeben, Material liest es (Pin vorhanden?
   zu prüfen) als Tiefenfaktor. Haken: das Blatt ist Konturpolygon-trianguliert (~1 Dreieck je Konturpunkt,
   keine Innenpunkte): braucht ein inneres Raster, sonst interpoliert die Tiefe über riesige Dreiecke.
B. SceneDepth + Depth Fade im Renderer (Plan §6): Tiefenkopie vor dem Translucent-Pass, 5 Backends,
   D3D11/D3D12/Vulkan erst mit NN-WS03; bricht die Metal/GL-zuerst-Reihenfolge.
- Schritt 2 (Ufer): Alpha-Rampe + Schaumsaum nach Tiefe (A), Kante nie hart. Nichts am Gitter ändern.
- Schritt 3 (Normalen/Wellen): feinerer Chop (Ref. 29, Skala unter 0,9 m, mit Entfernung ausblenden); 14 FBM/Pixel (§11.4), Budget prüfen.
- Schritt 4 (Transparenz): Deckkraft/Farbe aus Tiefe; Refraktion braucht SceneColor (nur B), mit A bleibt Alpha. Kaustik auf dem Grund = Decal/Licht.
- Guards: If rechnet beide Zweige (NaN-Schutz vor Divide/Power); Parameter-Slot 16 ist belegt.

### Werkzeug (nur Screenshot, in Commit)
`HE_DUMP_WATERRES`, `HE_DUMP_WATERSLOPE`, `HE_DUMP_WATERROUGH` im WATERLAKE-Witness (`EditorApplication.cpp`
~7728/7738, Standard unverändert), `scripts/water_baseline_shots.py`, `scripts/png_diff.py` (ohne PIL).
Falle: `HE_DUMP_LAKEEDIT` überschreibt `HE_DUMP_LAKEWATERRES` durch `ltc.water.res = 128` danach (~8203).
Nicht geprüft: D3D11/D3D12/Vulkan, `he_tests` (kein Code im Test-Pfad geändert), GPU-Zeit.
