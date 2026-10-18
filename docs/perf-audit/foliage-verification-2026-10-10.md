# Foliage: Verifikation, Messleiter vorher gegen nachher (Thema 163, Schritt 5)

Stand 10.10.2026 (Abend), Zweig `claude/instanced-foliage-und-lod-impostor-fuer-b-ume-gras-felsen` auf `00f72717` (Teil 2a und 2b, davor `origin/release/0.7.0`
gemergt, es gab nichts Neues). Bezug: `../foliage-instancing-lod-plan-2026-10-10.md` Abschnitt 9 (Messleiter) und 13 (Befund). Die Basis aus Schritt 2c steht in
`foliage-baseline-2026-10-10.md`, die erste Messung vorher gegen nachher aus Teil 2a in `foliage-clusters-2026-10-10.md`. Hier steht die Wiederholung zum Abschluss,
der Bildvergleich auf Metal und der volle `ctest`. Rohdaten: `raw-foliage-verify/` (`*.summary.json`, `*.conditions.txt`, `tables.md`, `scatter-ab.txt`, `ctest-summary.txt`).

## 1. Wie gemessen wurde, und was man davon nicht lesen darf

- **Vorher-Binary:** der selbstständige Deploy-Klon von `08a8938b` (Merge von `origin/release/0.7.0` auf dem Stand von Schritt 2c, also Messzeug und Zeuge, aber **ein Objekt je Pflanze**).
  Er ist derselbe Klon wie in `foliage-clusters-2026-10-10.md` (`strings` auf `libHorizonRendering.dylib`: kein `HE_FOLIAGE_CLUSTERS`). **Nachher-Binary:** `out/deploy/Editor` auf `00f72717`
  (`ninja` meldete nichts zu bauen, Release, `HE_ENABLE_SHADERC=ON`; `cmp` der Deploy-Bibliotheken `libHorizonRendering`, `libHorizonScene`, `libHorizonCore` und des `HorizonEditor` gegen den Build-Baum: identisch).
- **Ablauf:** `scripts/perf/foliage_ladder.sh` mit `he_perf_capture`, `--no-counters`, die Szenen aus dem Cache von 2c (flaches 400-m-Feld, eine Foliage-Schicht, eingebauter Würfel, Skala 0,5 bis 1,0, Kamera mitten im Feld).
  Je Fall **abwechselnd** Vorher, Nachher (`/tmp/fol5/ladder_ab.sh`), `drawDistance` 1e6 ("alle", ganzes Feld in Reichweite) und 100 m ("d100", rund 20 % des Feldes). Wiederholungen: 10k, 100k und 500k mit 100 m dreimal,
  500k "alle" zweimal, **1 Mio. nur einmal** (je ein Paar, "alle" nur 10 + 30 Frames, weil ein Vorher-Frame dort rund 2 s kostet). Warmup 40, Aufnahme 120 Frames (500k "alle": 30 und 60; 1 Mio. mit 100 m: 30 und 90).
  Verglichen wird wie vorher `Render` (Summe der CPU-Scope-Zeit je Frame), nicht `CPU p50` (enthält das Warten auf das Display).
- **Fremdlast, bitte vor jeder Zahl lesen.** Auf dem Gerät lief während der ganzen Messung ein Vollbau (acht `clang`-Prozesse, laut Queen der Build und die Messung von Thema 164) und der Haupt-Checkout war offen.
  **Load average 5,2 bis 13,6** (je Lauf in `raw-foliage-verify/*.conditions.txt`), nicht die 2 bis 3 der Basismessung. Die Läufe der Wiederholung 1 und 2 liefen bei **gesperrtem Bildschirm**, am Netzteil,
  Stromsparmodus aus, GPU-Auslastung vor dem Lauf 0 bis 14 %, **mit einer Ausnahme: das Paar 500k "alle" in Wiederholung 2** (Vorher-Lauf begann gesperrt und endete entsperrt, der Nachher-Lauf lief
  entsperrt bei 88 % fremder GPU-Auslastung vor dem Start; das benachteiligt eher "nachher", die 80 ms sind also eher zu hoch). **Wiederholung 3 und die 1-Mio.-Läufe liefen bei entsperrtem Bildschirm und mit fremder GPU-Last** (GPU-Auslastung vor dem Lauf 53 bis 100 %). Die Basismessung
  lief gesperrt, im Stromsparmodus (an), bei Load 2 bis 3. **Alle absoluten Millisekunden in diesem Dokument sind deshalb Zahlen unter Last** und nicht mit der Basismessung vergleichbar. Belastbar sind die Verhältnisse
  innerhalb eines abwechselnden Paares. Wie stark die Last die Zahlen verzerrt, zeigt 500k "alle": Vorher `Render` 465 ms (Wiederholung 1, Last 5 bis 9) gegen 2 132 ms (Wiederholung 2, Last 10 bis 12), nachher 22 gegen 80 ms.
  Bei 500k "alle" ist die Streuung über Faktor 4, die Spanne ist angegeben, kein Mittelwert gebildet.
- Wo die Bedingungen der Basismessung am nächsten kamen (Wiederholung 1 und 2, 100k "alle"), stimmt die Basis: `Render` vorher 149,7 und 159,7 ms gegen 153,8 ms in der Basismessung.

## 2. Ergebnis der Messleiter

`Render` p50 in ms je Frame, Median der Wiederholungen, in Klammern die Spanne. Vorher = ein Objekt je Pflanze (`08a8938b`), nachher = Cluster (`00f72717`). Verhältnis = Median der Verhältnisse je Paar.

| Instanzen | in Reichweite | Wdh. | `Render` vorher | `Render` nachher | Verhältnis (je Paar) |
|---|---|---|---|---|---|
| 10 000 | alle | 3 | 19,9 (13,3 bis 23,9) | 3,6 (2,9 bis 3,9) | 5,1 (3,7 bis 8,1) |
| 100 000 | alle | 3 | 159,7 (149,7 bis 246,7) | 12,3 (7,2 bis 13,0) | 12,3 (12,2 bis 34) |
| 100 000 | rund 20 % | 3 | 23,9 (13,9 bis 24,4) | 4,2 (1,9 bis 4,3) | 5,9 (5,6 bis 7,2) |
| 500 000 | alle | 2 | 465 und 2 132 | 22,3 und 79,9 | 20,8 und 26,7 |
| 500 000 | rund 20 % | 3 | 190,8 (126,9 bis 363,2) | 16,5 (10,6 bis 29,0) | 12,0 (11,6 bis 12,5) |
| 1 000 000 | alle | 1 (10 + 30 Frames) | 1 926 | 92,8 | 20,8 |
| 1 000 000 | rund 20 % | 1 | 287,5 | 17,7 | 16,2 |

Foliage-Anteil am Extract (`ExtractFoliage`) und ganzer Extract (`RenderExtractor::extract`), ms je Frame, Median (Spanne):

| Instanzen | in Reichweite | `ExtractFoliage` vorher | nachher | `extract` vorher | nachher |
|---|---|---|---|---|---|
| 10 000 | alle | 0,40 (0,21 bis 0,62) | **0,05** (0,04 bis 0,06) | 2,94 | 0,12 |
| 100 000 | alle | 6,32 (6,30 bis 6,87) | **0,08** (0,04 bis 0,09) | 22,6 | 0,19 |
| 100 000 | rund 20 % | 1,85 (0,77 bis 1,85) | **0,30** (0,11 bis 0,33) | 4,44 | 0,35 |
| 500 000 | alle | 10,1 und 73,6 | **0,07** und 0,06 | 55 und 334 | 0,13 |
| 500 000 | rund 20 % | 9,4 (4,3 bis 27,2) | 1,40 (0,62 bis 2,25) | 23,4 | 1,44 |
| 1 000 000 | alle | 61,6 | **0,08** | 347 | 0,16 |
| 1 000 000 | rund 20 % | 10,0 | 1,27 | 29,5 | 1,31 |

Zähler eines Frames und Speicher (Metal, Profiler):

| Instanzen | in Reichweite | `objects` vorher gegen nachher | `draws` vorher gegen nachher | `triangles` vorher gegen nachher | GPU p50 vorher gegen nachher (ms) | RSS max vorher gegen nachher (MB, Median) |
|---|---|---|---|---|---|---|
| 10 000 | alle | 10 004 gegen 173 | 3 gegen 3 | 51 176 gegen 61 124 (+19 %) | 6,3 gegen 5,1 | 219 gegen 266 |
| 100 000 | alle | 100 004 gegen 173 | 3 gegen 3 | 352 712 gegen 448 316 (+27 %) | 14,4 gegen 10,4 | 276 gegen 405 |
| 100 000 | rund 20 % | 19 719 gegen 47 | 3 gegen 3 | 74 468 gegen 107 768 (+45 %) | 5,5 gegen 5,6 | 367 gegen 357 |
| 500 000 | alle | 500 004 gegen 173 | **139 794 gegen 5** | 1 694 912 gegen 2 167 076 (+28 %) | 40 und 341 gegen 15 und 21 | 1 478 gegen 614 |
| 500 000 | rund 20 % | 98 267 gegen 47 | 3 gegen 3 | 300 080 gegen 466 760 (+56 %) | 16,6 gegen 9,5 | 546 gegen 501 |
| 1 000 000 | alle | 1 000 004 gegen 173 | **279 163 gegen 8** | 3 367 340 gegen 4 311 692 (+28 %) | 362 gegen 72 (fremde GPU-Last) | 2 757 gegen 1 094 |
| 1 000 000 | rund 20 % | 196 349 gegen 47 | 3 gegen 4 | 586 172 gegen 917 972 (+57 %) | 58 gegen 12,9 (fremde GPU-Last) | 849 gegen 586 |

Anmerkungen zu den Zahlen:

1. **Die GPU-Zeiten der 3. Wiederholung und der 1-Mio.-Läufe sind von fremder GPU-Last verfälscht** (z. B. 10k nachher 14,8 ms in Wiederholung 3 gegen 5,0 in 1 und 2). Die Spalte GPU ist nur als Richtung zu lesen:
   bis 100k im Rauschen, ab 500k deutlich niedriger. Dass die GPU nicht mehr Zeit braucht, obwohl `triangles` um 19 bis 57 % steigen (ein sichtbarer Bucket wird ganz eingereicht), ist an den Paaren mit 0 % Fremdlast vor dem Lauf zu sehen (100k "alle": 14,4 gegen 8,7 und 14,4 gegen 10,5).
2. **RSS ist nicht belastbar.** Bei 100k "alle" liegt er hier **nach** dem Umbau höher (276/318/271 gegen 405/399/436 MB), im Pass 2 von Teil 2a lag er **darunter** (461 gegen 396). Das Vorher-Binary schwankt zwischen den Läufen (hier und in 2a) um bis Faktor 1,7.
   Eindeutig ist nur der Sprung ab 500k (1,5 GB gegen 0,6 GB, 2,8 GB gegen 1,1 GB bei 1 Mio.), weil die Pflanzen-Objekte und ihre Kopien der `RenderWorld` entfallen. Eine Aufteilung ist, wie vorher, nicht gemessen.
3. **`ExtractFoliage` mit Reichweite 100 m hängt vom Randring ab**, nicht von der Gesamtzahl, aber der Ring wächst mit der Dichte: 0,30 ms bei 100k, 1,4 ms bei 500k, 1,27 ms bei 1 Mio. (Zähler `HE_FOLIAGE_STATS=1` bei 1 Mio. mit 100 m: 43 Cluster für 196 345 von 1 000 000 Instanzen, 24 Randbuckets mit 153 690 Einzeltests).
4. Die Vorher-Zahlen ab 500k "alle" sind Zahlen unter Last mit einer Streuung von Faktor 4. Das Verhältnis (20 bis 27) hängt davon ab, wie sehr die Last das Vorher-Binary trifft, das über 100 000 Draws mit CPU-Arbeit je Pflanze braucht.

## 3. Einmalige Streuzeit: der Umbau macht sie teurer (neuer Befund)

Der Zeuge `HE_DUMP_FOLIAGETEST` loggt die Zeit des Streuens (`scatter X ms`). Drei Wiederholungen, abwechselnd, Metal, unter derselben Fremdlast (Load 9 bis 12, `raw-foliage-verify/scatter-ab.txt`):

| Instanzen | vorher (ms) | nachher (ms) | Verhältnis (Mediane) |
|---|---|---|---|
| 500 000 | 65,9 / 75,8 / 69,1 (Median 69,1) | 163,0 / 99,4 / 138,6 (Median 138,6) | 2,0 |
| 1 000 000 | 123,0 / 104,7 / 83,0 (Median 104,7) | 228,8 / 186,8 / 150,9 (Median 186,8) | 1,8 |

Das stimmt mit den `tickWorld`-Warnungen in den Ladder-Logs überein (1 Mio. "alle": 48 gegen 106 ms, 1 Mio. mit 100 m: 49 gegen 110 ms; die Warnung erscheint nur über 16 ms, bei 100k und darunter ist der Unterschied im Rauschen).
Ursache nach Lektüre, nicht gemessen: der Scatter baut jetzt zusätzlich zu `cachedInstances` den Store mit den Buckets (Sortieren nach Zelle, terrain-lokale Matrizen, 64 Byte je Instanz). Es ist eine **einmalige Last je Neustreuen**, kein Frame-Preis.
Sie zählt aber doppelt, seit Teil 2b den Layer bei jeder Boden-Änderung neu streut: ein gehaltener Pinsel streut jeden Frame neu (Plan 12.2, dort mit "500k um 100 ms" geschätzt, jetzt gemessen eher 140 ms). **Kein Fehler im Sinn einer falschen Funktion, aber ein Preis,
den der Plan nicht genannt hat.** Nachfolger: Entprellung des Neustreuens (wie `g_pendingTerrainColliders`) und die Bucket-Sortierung billiger machen. Nicht Teil von 0.7 geändert.

## 4. Bildvergleich Metal

`python3 -I scripts/perf/foliage_pixel_ab.py --before /tmp/fol3/deploy_before --after out/deploy/Editor` (rohe BMPs, 1280 × 720, `HE_SKY_TIME=30`, Wolken aus, AA aus), in zwei Läufen (`raw-foliage-verify/ab_metal_*.txt`).
Vorher = derselbe Klon wie in 1. Ergebnis, **Pixelzahl identisch zu Abschnitt 11.3 des Plans** (die Läufe sind wiederholbar):

| Fall | `HE_FOLIAGE_CLUSTERS=0` (alter Pfad, neues Binary) | `=ordered` | Standard (Cluster) |
|---|---|---|---|
| A: 100k alle, Vorwärts | bitgleich | bitgleich | 751 Pixel (0,081 %), größter Kanalunterschied 86/255 |
| B: 100k alle, Deferred | bitgleich | bitgleich | 878 (0,095 %), 85/255 |
| C: 100k, 100 m, Kamera im Feld | bitgleich | bitgleich | 423 (0,046 %), 81/255 |
| D: 100k, tiefe Sonne | bitgleich | bitgleich | 579 (0,063 %), 90/255 |
| E: 100k, Skala 0,8 bis 1,6, Mesh-Id ohne Bounds | bitgleich | bitgleich | 571 (0,062 %), 111/255 |
| G: 500k alle (über der Instanz-Cap) | bitgleich | bitgleich | 3 730 (0,405 %), 79/255 |

Der alte Pfad und der Prüfmodus `ordered` reproduzieren das Vorher-Bild **Byte für Byte** (Skript endet mit `OK`). Der Standardpfad weicht an Schnittkanten ab, an denen zwei opake Flächen auf dieselbe Tiefe fallen und die Zeichenreihenfolge
entscheidet (Erklärung und Nachweis in Plan 11.3: ohne Bloom/SSAO/Schatten 35 Pixel). Ich habe das Bild A (Cluster) angesehen: Würfelfeld, Himmel, Schatten, nichts fehlt. **Nicht wiederholt:** OpenGL (Bild-A/B mit denselben Ergebnissen
stand in 2a und 2b, bitgleich beim eingebauten Material), Graph-Material (kein Vorher-Bild, siehe Plan 12.4), alle Backends mit TAA/AA an (4,3 % Abweichung unter AA=3 in 2a, dort erklärt).

## 5. Voller `ctest`

`cmake-build-release` (Release, `HE_ENABLE_SHADERC=ON`, `HEAD` `00f72717`), im Vordergrund, wegen des 600-s-Limits je Aufruf in zwei Hälften mit `ctest -I 1,130 -j4 --timeout 1500` und `ctest -I 131,257 -j4 --timeout 1500`:
**257 Tests, 255 bestanden, 2 übersprungen (`runtime_size_app_advanced`, `runtime_size_app_basic`), 0 rot.** Erste Hälfte 348 s (davon `test_material_graph` 348 s allein, unter Last), zweite 113 s.
Darin bestanden `test_foliage_cluster`, `test_foliage`, `test_culling`, `test_terrain_tools_ui`, `test_assimpimport`, `test_editor_help`, `test_material_graph`. Das ist **ein Lauf auf macOS (Apple clang)**. Windows, Linux und die Backends D3D11/D3D12/Vulkan prüft nur die CI: Lauf 38074831163 auf `00f72717` ist **grün** (macOS, Windows, Linux, Linux Vulkan/lavapipe; abgefragt mit `gh run view`, nicht darauf gewartet).
Die D3D11/D3D12-Zeilen der Foliage-Guards werden damit nur **übersetzt**, nicht ausgeführt; es lief nichts davon auf einem Gerät.

## 6. Ziele aus Plan 9, erreicht oder verfehlt

| Ziel | Ergebnis |
|---|---|
| Extract-Kosten der Foliage unabhängig von der Gesamtzahl | **Erreicht** für alle Pflanzen in Reichweite: `ExtractFoliage` 0,05 bis 0,08 ms bei 10k, 100k, 500k und 1 Mio. (vorher 0,4 bis 74 ms). Mit begrenzter Reichweite hängt er vom Randring ab (0,3 bis 1,4 ms), nicht vom Rest. |
| 100k platziert, 20 % sichtbar: Foliage-Extract unter 0,5 ms | **Erreicht** bei 100k (0,11 bis 0,33 ms). **Verfehlt** bei 500k (0,62 bis 2,25 ms) und 1 Mio. (1,27 ms): der Randring wächst mit der Dichte. Hebel: kleinere Buckets oder bucket-genaue Reichweite (Plan 11.4). |
| `draws` = Zahl der Mesh-Läufe, nicht der Instanzen | **Erreicht:** 3 bis 8 Draws, vorher bei 500k "alle" 139 794 und bei 1 Mio. 279 163. |
| Bild vorher gegen nachher | **Erreicht** für den alten Pfad und `ordered` (bitgleich in allen sechs Fällen); der Standardpfad ist nicht pixelgleich (0,05 bis 0,4 % der Pixel, erklärt). |
| `Render` je Frame | 5 bis 6 mal niedriger bei 10k und 100k mit 100 m, rund 12 mal bei 100k "alle" und 500k mit 100 m, 16 mal bei 1 Mio. mit 100 m, 21 bis 27 mal bei 500k und 1 Mio. "alle" (Zahlen unter Last, Verhältnisse je Paar). |
| Voller `ctest` mit shaderc ON | **255 grün, 2 übersprungen, 0 rot** (macOS). |
| Messleiter 1k/10k/50k/100k/200k aus Thema 153 | **Nicht gefahren**, nur 10k, 100k, 500k, 1 Mio. mit "alle" und 100 m. Kamera am Feldrand (sichtbarer Anteil 100 % gegen 20 % über die Kamera) und GI an wurden nicht gemessen. |

## 7. Nicht gemessen

OpenGL (nur Bilder aus 2a und 2b, keine Zeiten), D3D11, D3D12, Vulkan (Windows-Gleis, nie auf einem Gerät gelaufen), Graph-Material-Layer (ein Draw je sichtbarer Pflanze, Zeuge mit 2000 Pflanzen in 2b), GI an, Windows/Linux-CPU-Zeiten,
Wind, Kollision, FPS-Läufe (alle `--no-counters`-Läufe vergleichen `Render`, nicht FPS), eine Messung ohne Fremdlast, die Aufteilung von RSS, die Ursache der höheren Streuzeit (nur aus dem Code gelesen).
