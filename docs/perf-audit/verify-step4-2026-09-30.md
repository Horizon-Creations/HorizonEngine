# Vollbau, Tests, Sichtprüfung, GPU-Zeit vorher/nachher (Thema 103, Schritt 4), 30.09.2026

Zweig `claude/perf-sky-wolken-optimierungen-a1-a4-b7`, geprüft auf `b330a143` (A1–A3, A4, B7).
„Vorher“ ist die Merge-Basis mit main, `fe830e84`. Metal, Release, Apple M5.

## Ergebnis in vier Sätzen

1. **Vollbau grün** (alle Ziele, rc 0, 0 Fehler), **volle ctest-Suite 220/220 bestanden**, 54/54
   eingebettete Shader kompilieren.
2. **Sicht:** A4 ändert höchstens 1/255. A3 ändert nur das Horizont-Ausblendband der Wolken
   (Körnung, Einzelpixel bis 50/255), darüber und darunter bitgleich. A1 dunkelt den Boden unter
   Dome-Wolken sichtbar ab (bekannte, vor dem Merge zu entscheidende Standard-Änderung). Metal
   und GL verhalten sich gleich.
3. **GPU (Landschaft, 2840×1528, gesperrter Bildschirm, Paare im Wechsel):** Scene-Pass
   **−2,2 ms** p50, Shadow-Pass **−1,4 ms** p50, GPU-Frame **−3,8 ms** p50. Davon A4 (LUT an/aus,
   gleiches Binary) −0,9 ms Scene. Der Rest des Scene-Gewinns (~−1,0 ms) ist A3, der Shadow-Gewinn A1.
4. **B7-Zeuge:** Renderer-Init vorher 15,7 bis 19,1 s, nachher 1,1 bis 2,3 s (unter Fremdlast).

## 1. Build

| Schritt | Befehl | Ergebnis |
|---|---|---|
| Vorher-Build | 7 Quelldateien per `git checkout fe830e84 -- …`, `cmake --build out/build/macos-release -j8 --target HorizonEditor` | rc 0 |
| Vorher-Kopie | `out/deploy/Editor` nach `/tmp/he_before/…` kopiert, Exe und alle `libHorizon*.dylib` aus dem Build-Baum darübergelegt, `scripts/perf/selfcontain_deploy_copy.sh` (rpath `@loader_path`) | md5 `libHorizonRendering` vorher ≠ nachher |
| Zweig zurück | `git checkout HEAD -- src tests`, `touch` auf die 7 Dateien, `git status` sauber | |
| **Vollbau HEAD** | `cmake --build out/build/macos-release -j8` (alle Ziele) | **rc 0**, 0 `error:`, 22 Warnungen, keine aus dem Zweig-Diff (`MTLStorageModeManaged`-Deprecation, bestehend) |

Danach Exe und Dylibs aus dem Build-Baum ins Deploy kopiert und die Exe neu signiert
(der POST_BUILD-Deploy kopiert die Exe nicht zuverlässig).

## 2. Tests

- `ctest -j4 --output-on-failure` im Build-Baum: **100 % tests passed out of 220**, 121 s. Zwei
  davon `Skipped` (`runtime_size_app_basic`, `runtime_size_app_advanced`): Sie messen die
  Deploy-Ordner `AppBasic`/`AppAdvanced`, die lokal nicht gebaut sind. Absicht, kein Fehler.
- Abdeckung: 215 `--source-file`-Einträge in `CTestTestfile.cmake`. Die einzige Testquelle ohne
  Eintrag ist `test_d3d_shader_manager.cpp` (nur Windows). Damit laufen auch die Pins der Schritte
  1–3 (`test_culling.cpp`: Dome-Drift-Guard, Sky-View-LUT, SkyNoise3D-256³-Pin) und
  `test_sky_shader.cpp` mit.
- `scripts/validate_embedded_shaders.py`: 54 compiled, 0 failed.

## 3. Sichtprüfung

`scripts/he_shot.py`, 1280×720, `HE_SKY_TIME=30`, `RENDERPATH=0 AA=0` explizit. „Vorher“ über eine
Kopie von `he_shot.py` in `/tmp/he_before/scripts/` (dann zeigt `REPO` auf die Vorher-Kopie).
Je Szene vier Schüsse:

- (a) vorher, (b) nachher, (c) nachher mit `HE_SKY_LUT=0`, (d) nachher noch einmal (Kontrolle).
- (d) ist in allen drei Metal-Szenen **md5-gleich** zu (b), der Rauschboden ist 0.
- b−c isoliert A4, c−a isoliert A1+A3.

Differenz je Kanal in /255, Bänder zu 90 Zeilen (Werkzeug: `raw-verify-step4/imgdiff.py`, ohne
numpy/PIL). Bilder: `docs/perf-audit/shots-verify-step4/`.

**Szene T: Terrain unter Dome-Wolken** (`MOUNTAINTEST=before TOD=0.35 COVERAGE=0.5 CLOUDMODE=0
CAMX=0 CAMY=345 CAMZ=180 PITCH=-12`, Wolkenschatten ab Werk an)

| Vergleich | Mittel | max | Pixel ≠ | wo |
|---|---|---|---|---|
| A4 (c→b) | 0,005 | 1 | 1,4 % | nur Himmel, Boden 0 |
| A1+A3 (a→c) | 13,9 | 60 | 57 % | Himmel: nur Zeilen 15–210 (Ausblendband), Mittel ≤ 2,8, max 35. Boden: Helligkeit 197–204 → 164 |

Vorher liegt auf dem Terrain ein einzelner Fleck aus dem alten 3D-Schattenfeld, nachher ist das
Terrain gleichmäßig blaugrau abgedunkelt (Dome-Feld, 67 % Wolkenanteil bei coverage 0,5). Das ist
die in `clouds-a1-a3-2026-09-30.md` beschriebene Standard-Änderung. Differenzbild:
`diff_terr_a1a3.png` (Verstärkung ×8).

**Szene S: Himmel, Mittag** (`TOD=0.5 COVERAGE=0.5 CLOUDSHADOWS=0 CAMY=600 PITCH=12`)

| Vergleich | Mittel | max | Pixel ≠ | wo |
|---|---|---|---|---|
| A4 (c→b) | 0,006 | 1 | 1,8 % | über das Bild verteilt |
| A3 (a→c) | 0,36 | 50 | 14 % | nur Zeilen 270–539 (Ausblendband), Zeilen 0–269 und 540–719 bitgleich |

**Szene L: tiefe Sonne im Bild** (`TOD=0.27 YAW=90 COVERAGE=0.5 CLOUDSHADOWS=0 CAMY=600 PITCH=5`)

| Vergleich | Mittel | max | Pixel ≠ | wo |
|---|---|---|---|---|
| A4 (c→b) | 0,004 | 1 | 1,3 % | |
| A3 (a→c) | 0,19 | 18 | 13 % | nur Zeilen 180–449, sonst bitgleich |

Sonne und Aureole scharf, kein Ring, keine Stufe am Horizont (`low_b_after.jpg`).

**GL, Szene T** (`RHI=OpenGL`)

| Vergleich | Mittel | max | Pixel ≠ |
|---|---|---|---|
| Kontrolle (b→d, gleiches Binary) | 0,000 | 1 | 1 Pixel |
| vorher → nachher | 13,9 | 60 | 57 % (dasselbe Bandmuster wie Metal) |
| Metal (c, ohne LUT) ↔ GL nachher | 0,52 | 43 | Boden ≤ 1/255, Unterschied nur in den Wolken (bekannter Metal↔GL-Abstand) |

Der A1-Schatten stimmt auf Metal und GL bis auf 1/255 überein. A4 gibt es nur auf Metal.

## 4. GPU-Zeit vorher/nachher

`he_perf_capture.py --detailed --editor <Binary>`, Projekt-Kopie von `Test`,
`landscape.hescene`, `--cam 0,25,90,0,-0.25`, 2840×1528, 600 Frames nach 300 Warmup, 20 s Pause vor
jedem Lauf. Vorher- und Nachher-Binary im Wechsel. Rohdaten, Bedingungen je Lauf und Skripte:
`docs/perf-audit/raw-verify-step4/`.

**Bedingungen, bitte mitlesen:** Bildschirm **gesperrt** (alle Läufe), Stromsparmodus aus. Parallel
lief der Compile einer anderen Instanz (Load 3,4 bis 10,6). Die GPU-Auslastung vor dem Lauf lag bei
15 bis 63 %. Nicht geklärt ist, ob das Fremdlast war oder Nachlauf des eigenen vorigen Laufs (der
Wert vor dem ersten Lauf war 19 %). Die CPU- und FPS-Zahlen sind unter diesen Bedingungen nicht
auswertbar (cpu p50 14 bis 43 ms). Absolute GPU-Zeiten auch nicht (gesperrt = niedriger Takt). Maß
ist deshalb nur die Differenz eines Laufs zum Mittel seiner beiden Nachbarn der anderen Variante.

### Serie 1: vorher (B) gegen nachher (A), ms

| Lauf | Scene min | p10 | p50 | Shadow p50 | gpuMs p50 | Hänger | `EncodeSkyViewLut` CPU p50 |
|---|---|---|---|---|---|---|---|
| L-B0 | 4,54 | 6,34 | 6,78 | 2,062 | 11,66 | 2 | — |
| L-A1 | 2,14 | 3,54 | 5,12 | 0,860 | 9,61 | 2 | 0,0005 |
| L-B1 | 3,16 | 6,34 | 7,41 | 2,436 | 12,74 | 64 | — |
| L-A2 | 2,24 | 2,88 | 4,71 | 0,702 | 8,85 | 64 | 0,0004 |
| L-B2 | 2,84 | 3,80 | 7,04 | 2,142 | 13,05 | 2 | — |
| L-A3 | 1,84 | 3,03 | 5,74 | 0,861 | 9,55 | 9 | 0,0004 |
| L-B3 | 2,67 | 4,02 | 7,67 | 2,212 | 13,55 | 0 | — |
| L-A3b | 2,11 | 2,71 | 4,60 | 0,701 | 8,71 | 5 | 0,0005 |

Paar-Differenzen (nachher minus Nachbarmittel vorher), 6 Paare:

| Maß | Paare | Median |
|---|---|---|
| Scene p50 | −1,97 / −2,50 / −2,52 / −1,82 / −1,61 / −2,50 | **−2,23 ms** |
| Shadow p50 | −1,39 / −1,66 / −1,59 / −1,36 / −1,32 / −1,43 | **−1,41 ms** |
| gpuMs p50 | −2,59 / −3,51 / −4,04 / −3,85 / −3,75 / −4,42 | **−3,80 ms** |

Rauschboden (benachbarte Läufe desselben Binaries): Scene p50 A3→A3b 1,14 ms, B→B 0,37 bis
0,63 ms. Der Effekt liegt darüber, und alle Paare zeigen in dieselbe Richtung.

Nur die Nachher-Läufe haben den CPU-Scope `Metal::EncodeSkyViewLut`, die Vorher-Kopie läuft also
wirklich den alten Code (zweiter Zeuge neben der Init-Zeit). Er kostet 0,0005 ms, bei stehender
Sonne wird nicht neu gebacken.

### Serie 2: A4 allein, nachher-Binary, LUT an (A) gegen `HE_SKY_LUT=0` (AN), ms

| Lauf | Scene min | p10 | p50 | Shadow p50 | gpuMs p50 |
|---|---|---|---|---|---|
| L-AN1 | 3,54 | 4,37 | 7,18 | 0,862 | 10,90 |
| L-A6 | 2,37 | 3,16 | 5,60 | 0,866 | 9,86 |
| L-AN2 | 2,64 | 3,47 | 5,88 | 0,700 | 10,20 |
| L-A7 | 2,25 | 2,81 | 4,72 | 0,698 | 8,97 |
| L-AN3 | 2,61 | 3,17 | 5,71 | 0,702 | 9,58 |

LUT an minus aus: Scene p50 −0,93 / −0,72 / −1,07, Median **−0,93 ms**. Shadow +0,08 / +0,08 / 0,00
(unverändert, wie erwartet). Das passt zu Schritt 2 (−0,75 ms).

Zwei weitere Nachher-Läufe (L-A4, L-A5) stammen aus einem ersten Anlauf von Serie 2, in dem die
AN-Läufe wegen eines Quoting-Fehlers im Skript nicht starteten (in `conditions.txt` die ersten
AN-Zeilen ohne Summary). Sie sind als normale Nachher-Läufe gültig, gehen aber in keine
Paar-Differenz ein.

### Zuordnung

| Änderung | Pass | Einsparung (gesperrt, 2840×1528) | Schätzung vorher |
|---|---|---|---|
| A1 Dome-Wolkenschatten | Shadow | **−1,4 ms** | 0,2–0,4 ms (Schritt 1) |
| A3 Schrittbudget | Scene | **~−1,0 ms** (Scene gesamt minus A4) | 0,25–0,3 ms (Schritt 1) |
| A4 Sky-View-LUT | Scene | **−0,9 ms** | ~1 ms (Audit), −0,75 ms (Schritt 2) |
| A2 | — | ~0 (kein Bug, Schritt 1) | |
| B7 | — | GPU unverändert, Start −10 s CPU | |

A1 und A3 sparen mehr als in Schritt 1 aus Schritt- und Fetch-Zählungen geschätzt. Der Shadow-Pass
lag vorher bei 2,1 bis 2,4 ms, das Audit hatte den ganzen Wolkenschatten mit 0,57 ms angesetzt
(entsperrt, 1718×884). Bei gesperrtem Bildschirm taktet die GPU niedriger, alle Zeiten sind also
aufgebläht, die Verhältnisse sollten halten. Die A3-Zahl ist eine Differenz zweier Messungen
(Serie 1 minus Serie 2) und damit unsicherer als die beiden anderen.

## Offen

- Nachmessung am **entsperrten** Mac ohne Fremdlast, damit die Zahlen absolut gelten und die FPS
  zählen.
- Die Entscheidungen aus den Schritten 1–3 bleiben: Dome-Schatten ab Werk an (sichtbar dunklerer
  Boden), keine LUT auf GL/D3D/Vulkan, Fußabdruck der Rauschtextur (~1,5 ms) nur gegen Optik.
- `skyonly*.hescene` nicht gemessen: Unter Sperre bimodal (Drawable-Pacing, siehe Schritt 2/3), dort
  gibt es nichts, was die Landschaft nicht schon zeigt.
