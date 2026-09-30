# Sky-View-LUT (A4, Thema 103, Schritt 2), 30.09.2026

Zweig `claude/perf-sky-wolken-optimierungen-a1-a4-b7`, Commit `cf3b4ba0`. **Nur Metal.** GL, D3D11,
D3D12 und Vulkan rechnen den Himmel weiter pro Pixel (der GL-Himmel in `SkyShaderSource.h` wird für
D3D/Vulkan kreuzkompiliert, eine neue Textur dort ließe sich hier auf keinem dieser drei Backends
prüfen).

## Was gebaut ist

- `atmoScatter` hängt bei fester Kamerahöhe (200 m) nur von Blick- und Sonnenrichtung ab. Für eine
  Sonne ist das eine 2D-Funktion: Elevation × Azimut relativ zur Sonne (spiegelsymmetrisch, 0..π).
- `skyViewLutFragment` backt sie in zwei 256×128-RGBA16F-Ziele: Ziel 0 = Rayleigh-Term mit Phase plus
  Mehrfachstreu-Füllung, Ziel 1 = Mie-Term **ohne** Phase. `atmoScatterLut` multipliziert die
  Mie-Phase pro Pixel mit dem exakten Winkel. So wird die scharfe Mie-Keule (g = 0,76) nicht
  interpoliert, und die Aureole um die Sonne bleibt so scharf wie vorher.
- Zeilen sqrt-verteilt (dicht am Horizont). Die Mitte von Zeile 0 liegt genau auf `skyColor`s
  Horizont-Klemme (`dir.y` 0,004), die letzte Zeile auf dem Zenit. Spalten: Azimut 0 und π auf
  Texelmitten, `atan2` statt `acos`/`asin` (gut konditioniert an Zenit und Sonnenachse).
- `atmoScatter` ist in `atmoIntegrate` (Integral, einmal geschrieben) + `atmoPhaseRayleigh/Mie` +
  `atmoMultiFill` zerlegt, `skyColor` in den Wrapper + `skyColorAtmo`. Nur `skyFragment` liest die
  LUT. Nebel (`fogCol`), der Dunst der 3D-Wolken und die CPU-Kopie (`SkyEnvBake.h`) bleiben analytisch.
- Gebacken wird auf Frame-Ebene nach dem Wolkenschatten, vor dem Szenen-Encoder, und nur wenn die
  Sonne sich um ≥ 1e-4 bewegt hat (dieselbe Schwelle wie `UpdateSkyEnvCube`). Bei einem Tageszyklus
  sind das 32 768 Texel pro Sonnenschritt gegen ~1–4 M Himmelspixel pro Frame.
- `EncodeSky` liest die LUT nur, wenn sie für genau die Sonne gebacken ist, mit der er zeichnet.
  Sonst (LUT aus, noch nicht gebacken, Extraktion übersprungen) integriert der Shader wie bisher.
  Die Welt-Vorschau (eigener Command Buffer, keine Reihenfolge zum Bake) bleibt beim Integral.
- `HE_SKY_LUT=0` schaltet die LUT ab. Damit laufen A/B-Vergleiche mit demselben Binary.

## Sichtprüfung

`he_shot.py`, Metal, 1280×720, `HE_SKY_TIME=30 CLOUDSHADOWS=0 AA=0`, jeweils `HE_SKY_LUT=0` gegen
Standard. Kontrollschuss (zweimal `HE_SKY_LUT=0`): **bitgleich** (md5 gleich), der Rauschboden ist 0.

| Szene | Mittel /255 | max /255 | Pixel ≠ |
|---|---|---|---|
| Mittag, `COVERAGE=0 CAMY=600 PITCH=12` | 0,015 | 1 | 4,3 % |
| TOD 0,27 (tiefe Sonne), YAW 0/90/180/270, `PITCH=5` | 0,013–0,015 | 1 | 3,8–4,4 % |
| TOD 0,25 (Sonnenuntergang, Sonne bei YAW 90 im Bild) | 0,009–0,013 | 1 | 2,8–3,8 % |
| TOD 0,23 (Dämmerung) | 0,000–0,001 | 1 | 0,01–0,17 % |
| TOD 0,0 (Nacht) | 0 | 0 | 0 % |
| Zenit, `PITCH=85`, TOD 0,5 und 0,35 | 0,018–0,021 | 1 | 5,4–6,3 % |
| Deferred (`RENDERPATH=1`), Mittag | 0,015 | 1 | 4,3 % |
| Dome-Wolken `COVERAGE=0.5`, TOD 0,5 und 0,27 | 0,004–0,006 | 1 | 1,3–1,8 % |
| 3D-Wolken `CLOUDMODE=1`, TOD 0,27 | 0,007 | 1 | 2,2 % |
| Terrain (`MOUNTAINTEST=before`), TOD 0,35 | 0,007 | 1 | 2,1 % |

Überall höchstens 1/255 (Rundung nach Tonemapping). Am Sonnenuntergang mit Sonne und Aureole im Bild
gibt es keinen Ring und keine Stufe am Horizont. Dass jede Tagszene überhaupt abweicht, belegt, dass
der LUT-Pfad wirklich läuft.

## GPU-Zeit

`he_perf_capture.py --detailed`, Projekt-Kopie von `Test`, `--cam 0,25,90,0,-0.25`, 2840×1528,
600 Frames nach 300 Warmup, Läufe abwechselnd an/aus. Rohdaten: `docs/perf-audit/raw-a4/`.

**Bedingungen:** Bildschirm **gesperrt** (`CGSSessionScreenIsLocked=Yes`), Stromsparmodus aus, Load
0,9–1,4, GPU vor dem Lauf 18 % fremd belegt. Laut `perf-measurement-on-m5` taktet die GPU bei
gesperrtem Bildschirm niedriger, und die FPS sind nicht aussagekräftig. Die Zahlen sind deshalb als
Vergleich an/aus unter gleichen Bedingungen zu lesen, nicht als absolute Werte.

Scene-Pass, exklusive GPU-Zeit, ms (min / p10 / p50):

| Lauf | Scene | gpuMs p50 | delta p50 | Hänger |
|---|---|---|---|---|
| P0 landscape, aus | 5,26 / 5,61 / 6,58 | 10,00 | 14,07 | 1 |
| P1a landscape, an | 5,18 / 5,39 / 5,83 | 10,00 | 14,03 | 3 |
| P1b landscape, aus | 5,26 / 5,55 / 6,60 | 10,16 | 14,24 | 2 |
| P2a landscape, an | 4,17 / 5,19 / 5,55 | 9,38 | 13,42 | 1 |
| P2b landscape, aus | 3,47 / 3,99 / 6,17 | 9,78 | 13,79 | 1 |
| P3a landscape, an | 2,58 / 3,64 / 5,46 | 9,22 | 13,24 | 4 |
| S1a Himmel ohne Wolken, an | 0,36 / 0,56 / 0,73 | 2,88 | 13,88 | 64 |
| S1b Himmel ohne Wolken, aus | 2,07 / 2,08 / 4,21 | 6,64 | 12,07 | 64 |
| S2a Himmel ohne Wolken, an | 0,36 / 0,56 / 0,73 | 3,13 | 13,93 | 64 |
| S2b Himmel ohne Wolken, aus | 2,07 / 2,08 / 3,83 | 6,23 | 11,19 | 64 |

- **Landschaft** (`landscape.hescene`, Dome-Wolken coverage 0,5, Terrain; GPU-gebunden, gleichmäßiger
  Takt): Scene p50 zwischen je zwei aufeinanderfolgenden Läufen (aus/an im Wechsel) −0,75 / −0,77 / −1,05 /
  −0,62 / −0,71 ms, Median **−0,75 ms** bei 2840×1528. Die min-Werte driften über die Serie in beiden
  Varianten nach unten (Takt), p50 ist das stabile Maß. Das ist die Zahl im Kontext der Audit-Szene;
  die Audit-Schätzung „~1 ms“ war für 1718×884 angesetzt, eine saubere Nachmessung am entsperrten Mac
  steht aus.
- **Himmel ohne Wolken** (`skyonly_noclouds.hescene`, ganzes Bild Himmel, 4,3 M Pixel): Scene p50
  4,0 → 0,73 ms (−3,3 ms), min 2,07 → 0,36 ms. Das ist eine **Obergrenze**: Bei so leichter Last
  taktet die GPU wahrscheinlich niedriger, und die „aus“-Läufe sind bimodal (min 2,07, p50 ~4).
  Zur Größenordnung passt es (Audit: 1,33 ms für 1,52 M Pixel, hochgerechnet ~3,8 ms für 4,3 M).
- **FPS in der Himmel-Szene nicht auswertbar:** Beide Varianten laufen im Muster kurz/lang
  (~6 ms, dann ~68 ms; je 64 Hänger). Das ist das Drawable-Pacing bei gesperrtem Bildschirm, wenn ein
  Frame fast nichts kostet. Mit der billigeren LUT-Variante tritt es häufiger auf (FPS-Mittel 32
  statt 52, `NextDrawable` 13,9 s statt 5,1 s über 600 Frames). Mit der LUT kommt pro Frame nichts
  Blockierendes dazu: `Metal::EncodeSkyViewLut` 0,0004 ms p50 (kein Neubacken bei stehender Sonne),
  sonst nur zwei Texturbindungen. Die GPU-gebundene Landschaft läuft in beiden Varianten
  gleichmäßig (596 von 600 Frames zwischen 12 und 24 ms). Nachmessen am entsperrten Mac, wenn die
  FPS zählen sollen.

## Tests

`tests/test_culling.cpp`, „Sky-View LUT: Metal's host code and kSkyMSL agree on its size and
horizon“: LUT-Größe Host ↔ MSL, alle Horizont-Klemmen gleich, `kSkyLutEl0 = asin(Klemme)`,
Mie-Aufteilung (Bake ohne Phase, Lookup mit Phase, gleiche Belichtung 20,0 wie `atmoScatter`).
Negativkontrolle: MSL-Breite 255 bzw. Klemme 0,005 im Lookup machen ihn rot.
`he_tests --source-file='*test_culling.cpp,*test_sky_shader.cpp'`: 56/56 grün.

Das MSL wird zur Laufzeit kompiliert (`newLibraryWithSource`, ein Fehler bricht den Start ab). Alle
Schüsse oben sind damit auch der Kompilierbeleg. Offline (`xcrun metal`) ging nicht, auf diesem
Rechner fehlt die Metal-Toolchain.

## Offen

- GL/D3D11/D3D12/Vulkan: Himmel weiter pro Pixel. Für GL wäre ein `#ifdef HE_SKY_VIEW_LUT` im
  `SkyShaderSource.h`, das nur GL setzt, der Weg, der D3D/Vulkan nicht berührt.
- `fogCol = skyColor(...)` im Szenen-Shader (`MetalRenderer.mm`, `kUnlitMSL`) rechnet dasselbe
  Integral pro **Geometrie**-Pixel, wenn Nebel aktiv ist. Kandidat, dieselbe LUT mitzubenutzen.
- FPS-Messung am entsperrten Mac.
