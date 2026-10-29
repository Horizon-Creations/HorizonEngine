# Auto-LOD: Baseline und Sofortmassnahmen in der Foliage-Testszene (Thema 331, Schritt 1)

Stand 29.10.2026, Metal auf dem M5 (MacBook Air). **Kein Engine-Code**, nur Messung. Die Zahlen sind
der Massstab fuer die Schritte 2 bis 7 und fuer das Ziel-Budget-Gespraech.

## 1. Kurzfassung

| | Baseline | beste handgemachte Variante (v5, alles zusammen) | Boden (keine Foliage) |
|---|---|---|---|
| Dreiecke im Hauptpass | **47,75 Mio** | 6,30 Mio | 0,10 Mio |
| GPU-ms gesamt (Pass-Lauf, RenderScale 2) | **121,0** | 57,6 | 20,9 |
| fps im Pass-Lauf (Zaehler an, RenderScale 2) | 8,0 | 16,0 | 39,8 |
| fps Standard-RenderScale, `--no-counters` | **13,6 / 13,7** | 44,0 / 41,3 | 60,1 (Display-Raster) |

* Der Mensch sah 8 fps. Das **reproduziert sich nur im Pass-Lauf** (RenderScale 2, Zaehler an: 8,03 / 7,96 fps);
  bei Standard-RenderScale ohne Zaehler sind es 13,6 fps. Welche Einstellung der Mensch hatte, ist unbekannt.
* **Dreiecke sind nicht die ganze Kostenkurve.** Schatten- und SSAO-Pass folgen den Dreiecken (33,3 -> 5,2 ms,
  12,3 -> 2,7 ms), der Hauptpass nicht: minus 87 % Dreiecke (v3) bringt nur minus 18 % Scene-Zeit
  (73,6 -> 60,6 ms). Bei RenderScale 1 (ein Viertel der Pixel) faellt die Scene-Zeit der Baseline von 73,6 auf 31,4 ms,
  Schatten (33,0) und SSAO (11,3) bleiben stehen. Der Hauptpass haengt also an den Pixeln (Cutout-Ueberzeichnung der
  nahen Pflanzen), nicht an den Eckpunkten. Das ist ein **Befund aus zwei Skalen, nicht bewiesen**; ein Ueberzeichnungs-
  oder Early-Z-Test steht aus (Schritt 5 sollte ihn zuerst machen, siehe Handoff).
* Die guenstigste einzelne Massnahme in GPU-ms ist die Pflanzen-LOD-Kette (v3, minus 51 ms), die guenstigste in
  Aufwand ist Schatten/AO aus fuer kleine Pflanzen (v1, minus 38 ms, ein Schalter je Layer, keine Optik im Hauptpass).

## 2. Bedingungen (gelten fuer jede Zahl unten)

* Editor: Klon von `out/deploy/Editor` des Haupt-Checkouts (gebaut 29.10. 09:12, HEAD dort vermutlich `31d6253c`;
  ob unbestaetigte Aenderungen drin waren, ist nicht pruefbar), `selfcontain_deploy_copy.sh` angewandt (eigene dylibs).
  Projekt: APFS-Klon von `~/HorizonEngineTestAssets/foliage-test-project`; das Original blieb unberuehrt
  (`--scene` ueberschreibt die Startszene des Projekts dauerhaft, deshalb der Klon).
* Alle Laeufe: `--cam 0,2.5,-24,3.14159,-0.15` (am Rand des 4x3-Rasters, blickt ueber das Feld), `--env HE_SKY_TIME=40000`,
  `--warmup 300 --frames 600`, Fenster 2840x1528, `caffeinate -d -u -i`, kein Editor-Strayprozess.
  **Jeder Lauf nennt seine Szene mit `--scene`** (ein Lauf ohne `--scene` hat einmal die Variante des Vorlaufs gemessen,
  siehe Fallen; dieser Lauf ist verworfen und wiederholt).
* **Pass-Laeufe**: `--detailed --set RenderScale=2`, Pass-p50 aus `gpuPassPercentiles`. **FPS-Laeufe**: `--no-counters`,
  Standard-RenderScale. Eine Aufnahme je Aufruf.
* Maschine: **Akkubetrieb**, Bildschirm nicht gesperrt (`CGSSessionScreenIsLocked` fehlt), Stromsparmodus aus.
  **Fremdlast war hoch**: Load Average 3 bei den ersten Laeufen, 15 bis 50 in der Mitte, zuletzt 133 (andere Bees bauten);
  die GPU-Auslastung im Leerlauf vor einem Lauf schwankte 26 bis 98 % (WindowServer, CLion, Hive-App). Jede Zeile hat ihre
  Last in `perf-raw-2026-10-29/conditions.txt`. Trotzdem war die Streuung klein (Abschnitt 5): die Szene ist GPU-gebunden.
* Taktzeuge (Tonemap-p50 bei RenderScale 2): 0,376 bis 0,413 ms in allen Zeilen v0 bis v5 (+-5 %, v1 bis +10 %);
  die Effekte (10 bis 60 ms) sind weit groesser. **Ausnahme**: bei RenderScale 1 ist Tonemap in der v6-Zeile 0,210 gegen
  0,143 (Baseline) und 0,143 (v5): diese Zeile ist **nicht vergleichbar**, die RS1-Foliage-Kosten sind daher nur grob.
* Dreiecke: `counters_midframe.tris`. Der Zaehler steht in `EncodeScene`/`EncodeGBuffer` (MetalRenderer.mm 14399 bis 16912),
  zaehlt also **nur den Hauptpass**. **Dreiecke je Schatten- und SSAO-Pass werden von der Engine nicht gezaehlt**; ersatzweise
  steht die Zahl der Caster-Batches aus der Logzeile `shadow pass instanced (first run: N casters)` / `SSAO pre-pass instanced
  (first run: N objects)` in der Tabelle (Batches der ersten Frames, kein Dreieckszaehler).
* GI war aus (GIAccel/GIProbes/GIShadow 0,00 ms in jedem Lauf): **GI-Vorpass nicht gemessen**.

## 3. Szene und Varianten

12 Foliage-Layer, je ein 12-m-Terrain im 4x3-Raster, 1900 Instanzen (Dichte x 144 m2), 24 Draws im Hauptpass in der Baseline.
Szenen: `~/HorizonEngineTestAssets/foliage-test-project/variants/*.hescene` (erzeugt von `scripts/perf/foliage_variants.py`
aus `docs/perf-audit/scenes/foliage-test-base.hescene`; `v0_base` ist die semantisch gleiche, neu serialisierte Basis).
Groessenklassen nach der groessten Bounding-Box-Seite der glTF-Meshes (Knotenskalierung nicht beruecksichtigt):

| Klasse | Pflanzen (groesste Seite) | Layer-drawDistance Basis |
|---|---|---|
| klein < 0,7 m | celandine 0,31, dandelion 0,26, grass_medium 0,34, nettle 0,22, periwinkle 0,41, shrub_03 0,40, shrub_04 0,58, weed_02 0,29 | 40 bis 50 |
| mittel < 1,5 m | anthurium 1,14, fern_02 0,99 | 60 |
| gross | flower_gazania 2,11, shrub_02 2,34 | 60 / 80 |

| Variante | Aenderung (gegenueber v0, jede allein) |
|---|---|
| v1 shadowao | klein: `castsShadow=false`, `contributesAO=false` (explizit geschrieben) |
| v2 shadowdist | alle Layer: `shadowDistance=15` |
| v3 lods | LOD-Kette je Klasse: klein `8 m eigenes Mesh Dichte 0,5` dann `16 m Kreuzkarte Dichte 0,5`; mittel `15 m / 0,7`, `28 m Kreuzkarte / 0,7`; gross `30 m Kreuzkarte / 1,0` |
| v4 drawdist | drawDistance klein 25, mittel 40, gross unveraendert |
| v5 all | v1 (klein) + v2 (mittel/gross) + v3 + v4 |
| v6 nofoliage | alle Layer `layerVisible=false` (Boden: Terrain, Himmel, Wolken) |

Wirkung bestaetigt: Szenendatei enthaelt die Schluessel (geprueft per Skript, alle 12 Layer), und die Logzeile aendert sich
(Spalte Caster/AO), die Dreiecke aendern sich nur dort, wo Mesh oder Reichweite sich aendern.

## 4. Tabellen

### 4.1 Pass-p50 in ms, RenderScale 2, Mittel aus zwei Laeufen (Min bis Max)

| Variante | GPU gesamt | Scene | Shadow | SSAO | Tris Hauptpass | Draws | Caster / AO-Objekte (Log) | fps (Zaehler an) |
|---|---|---|---|---|---|---|---|---|
| v0 Baseline | 121,0 (120,1-121,9) | 73,6 (72,7-74,6) | 33,3 (33,3-33,4) | 12,3 (12,3-12,3) | 47,75 M | 24 | 144 / 144 | 8,0 |
| v1 shadowao | 82,6 (81,4-83,7) | 73,6 (72,7-74,5) | 4,8 (4,7-5,0) | 2,3 (2,2-2,4) | 47,75 M | 24 | 21 / 21 | 11,4 |
| v2 shadowdist | 100,7 (97,4-104,1) | 75,4 (73,1-77,7) | 10,4 (10,2-10,5) | 13,3 (12,4-14,2) | 47,75 M | 24 | 288 / 144 | 9,4 |
| v3 lods | 70,1 (70,1-70,2) | 60,6 (60,3-60,8) | 5,2 (5,0-5,4) | 2,7 (2,7-2,8) | 6,30 M | 27 | 665 / 665 | 13,2 |
| v4 drawdist | 96,4 (96,3-96,4) | 65,2 (64,9-65,5) | 21,4 (21,1-21,6) | 8,1 (7,9-8,3) | 29,71 M | 23 | 65 / 65 | 9,8 |
| v5 all | 57,6 (57,3-57,9) | 54,0 (53,8-54,3) | 0,8 (0,8-0,8) | 1,0 (1,0-1,0) | 6,30 M | 26 | - / 84 | 16,0 |
| v6 nofoliage (1 Lauf) | 20,9 | 17,5 | 0,8 | 0,9 | 0,10 M | 12 | - / - | 39,8 |

Bloom 0,8 bis 0,9 und Present 0,27 ms sind in allen Zeilen gleich. "GPU gesamt" ist die Summe der Passes (`detailedSumOverGpu` 1,0).
Die Zeile `-` heisst: die Logzeile wurde nicht geschrieben (v5: Schatten nur fuer die grossen Layer, die ab 22 m Abstand ausserhalb
der 15 m liegen; Shadow 0,8 ms ist der Boden wie in v6).

### 4.2 Abstand zur Baseline (Mittel, Pass-Lauf)

| Massnahme | GPU gesamt | Scene | Shadow | SSAO | Dreiecke |
|---|---|---|---|---|---|
| v1 Schatten+AO aus (klein) | **-38,4 ms (-32 %)** | +-0 | -28,5 | -10,0 | +-0 |
| v2 shadowDistance 15 | **-20,3 ms (-17 %)** | +1,8 (Rauschen) | -22,9 | +1,0 (Rauschen) | +-0 |
| v3 LOD-Ketten + Kreuzkarte + Ausduennung | **-50,9 ms (-42 %)** | -13,0 | -28,1 | -9,6 | **-41,45 M (-87 %)** |
| v4 drawDistance je Groesse | **-24,6 ms (-20 %)** | -8,4 | -11,9 | -4,2 | -18,04 M (-38 %) |
| v5 alles zusammen | **-63,4 ms (-52 %)** | -19,6 | -32,5 | -11,3 | -41,45 M (-87 %) |

Die Einzeleffekte sind **nicht additiv** (Summe 134 ms gegen 63 ms gemessen): dieselben Instanzen sparen sich mehrfach.

### 4.3 FPS bei Standard-RenderScale, `--no-counters`, zwei Laeufe je Zeile

| Variante | fps (Lauf 1 / 2) | p50-Frametime ms | 1 %-Low fps |
|---|---|---|---|
| v0 Baseline | 13,6 / 13,7 | 67,0 / 67,2 | 8,3 / 9,4 |
| v1 shadowao | 26,8 / 26,4 | 33,4 / 33,7 | 14,4 / 13,6 |
| v2 shadowdist | 19,0 / 19,0 | 50,1 / 50,1 | 10,8 / 10,6 |
| v3 lods | 34,0 / 34,1 | 33,1 / 33,1 | 16,6 / 17,7 |
| v4 drawdist | 18,0 / 18,3 | 50,2 / 50,3 | 11,3 / 10,7 |
| v5 all | 41,3 / 44,0 | 16,9 / 17,3 | 20,0 / 22,0 |
| v6 nofoliage | 60,1 (1 Lauf) | 16,7 | 44,6 |

**Achtung**: die p50-Frametime liegt hier bei Vielfachen von 16,7 ms (67,0 = 4, 50,1 = 3, 33,4 = 2, 16,7 = 1 Display-Takt).
Trotz `vsync off` rastet das Fensterbild im 60-Hz-Takt ein. Mittelwerte wie 13,6 fps sind also Mischungen aus Stufen, und eine
Verbesserung unter einer Stufe kann ganz verschwinden. **Die kontinuierliche Zahl ist der Pass-p50 aus 4.1.** Das v6-Ergebnis
60,1 fps ist die Display-Obergrenze, nicht die Engine-Obergrenze.

### 4.4 RenderScale 1, Pass-p50 (je ein Lauf, Zaehler an)

| Variante | GPU | Scene | Shadow | SSAO | Tonemap | fps |
|---|---|---|---|---|---|---|
| v0 | 76,4 | 31,4 | 33,0 | 11,3 | 0,145 | 12,1 |
| v5 | 20,8 | 18,8 | 0,8 | 0,3 | 0,143 | 36,0 |
| v6 | 16,8 | 13,9 | 1,3 | 0,5 | **0,210** | 44,4 (nicht vergleichbar) |

## 5. Streuung und Rauschen

* Baseline Pass-Lauf: GPU 121,9 / 120,1 ms (1,5 %), Scene 74,6 / 72,7, Shadow 33,4 / 33,3, SSAO 12,3 / 12,3. FPS-Lauf: 13,62 / 13,68 (0,4 %),
  obwohl der erste bei Load 19, der zweite bei Load 41 lief.
* Groesstes Paar der Varianten: v2 GPU 97,4 / 104,1 (6,7 ms; Load 10 gegen 47). v5 FPS-Lauf 41,3 / 44,0 (6 %). Sonst unter 2,5 ms
  beziehungsweise unter 2 %.
* **Die Streuung (hoechstens 7 ms) liegt unter jedem Effekt der Maske v1, v2, v3, v4, v5 (20 bis 63 ms).** Nicht unterscheidbar
  vom Rauschen sind nur die Scene-Zeiten von v1 und v2 (erwartet unveraendert: 73,6 und 75,4 gegen 73,6) und SSAO bei v2.
* Je Zeile liegen nur zwei Laeufe vor (v6 und RS1 je einer): eine Aussage wie "v2 spart 20 ms" gilt auf +-5 ms, nicht darunter.

## 6. Qualitaetspreis (Bilder: `docs/perf-audit/img/auto-lod-baseline-base.jpg` und `-v5.jpg`, Frame 150, gleiche Kamera)

* **Die Kreuzkarte ist nur ein Platzhalter.** Es gibt das Mesh (`kEngineCrossCardMeshId`, `{0x108, 1}`, `Engine/Meshes/CrossCard.hasset`,
  zwei gekreuzte 1x1-m-Quads mit Fuss bei y=0), aber **kein Engine-Material mit einem Pflanzenbild**. Die Stufe nimmt das Material des Layers,
  und das ist die Poly-Haven-Diffuse (Atlas der Blaetter, nicht ein Bild der Pflanze). In v5 stehen daher in den hinteren Reihen
  1 m grosse Karten mit Atlas-Bild statt Pflanzen (siehe Bild). Fuer die Zeitmessung gleich teuer wie ein gebackener Impostor,
  optisch **nicht brauchbar**; es braucht Schritt 4 (Impostor baken).
* v3/v5 duennen aus: ab 8 m (klein) bleiben nur 50 % der Pflanzen, ab 16 m 50 % als Karten. Sichtbar als lichtere Wiese.
* v4/v5 kuerzen die Sicht der kleinen Pflanzen auf 25 m (Abbruchkante sichtbar). v2/v5 lassen Schatten ab 15 m weg.
* Nicht gemessen: Bildqualitaetsmetrik, Bewegung (Popping an den LOD-Grenzen), Wind.

## 7. Fallen dieses Schritts

* `he_perf_capture.py --scene` ueberschreibt `Content/StartupScene.hescene` des Projekts **dauerhaft**. Ein Lauf ohne `--scene`
  misst die zuletzt kopierte Variante (hier: v6 ohne Foliage, 60 fps, 0,10 M Dreiecke, aufgefallen an den 17 s Laufzeit).
  Immer `--scene` setzen, am besten gegen einen Projektklon.
* Ein Editor-Klon (`cp -cR` des Deploy-Ordners) braucht `scripts/perf/selfcontain_deploy_copy.sh`, sonst laedt er die dylibs des Build-Baums
  (Memory copied-deploy-loads-build-tree-dylibs). Das Skript schreibt `dumps/` und `HorizonEngine.log` neben die `--editor`-Binary.
* `foliage_variants.py` schreibt auch `v0_base` neu (semantisch gleich); die Szene im Testprojekt bleibt das Original.
* Die Logzeile `shadow pass instanced` fehlt, wenn kein Layer mehr im Schattenbereich liegt (v5, v6).

## Handoff

- **Topic / step:** 331 Auto-LOD, Schritt 1 von 7 (Baseline und Sofortmassnahmen)
- **Branch / commit:** `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`, Commit siehe `git log` (pushed: nein, Queen merged)
- **Done:** Baseline und fuenf Massnahmen-Varianten gemessen, je zwei Laeufe; Tabellen oben; Boden ohne Foliage; Zeugenbilder.
- **Files touched:** `docs/auto-lod-baseline-2026-10-29.md` (diese Doku), `scripts/perf/foliage_variants.py` (Varianten-Generator),
  `docs/perf-audit/scenes/foliage-test-base.hescene` (Basisszene), `docs/perf-audit/img/auto-lod-baseline-{base,v5}.jpg`. Kein `src/`.
  Ausserhalb des Repos: `~/HorizonEngineTestAssets/foliage-test-project/variants/v0..v6*.hescene` und `perf-raw-2026-10-29/`
  (29 Summaries, Logs, `conditions.txt`, `cap.sh`, `tab.py`).
- **Verified:** Alle Zahlen aus `*.summary.json` (`he_perf_capture.py`); Variantenwirkung per Skript (Schluessel) und Logzeile;
  Tonemap-Zeuge je Zeile; Baseline zweimal in verschiedener Last; Bilder angesehen.
- **Not verified:** Dreiecke je Schatten-/SSAO-Pass (Engine zaehlt sie nicht); GI-Vorpass (GI war aus); OpenGL/D3D/Vulkan;
  Ueberzeichnung als Ursache der Hauptpass-Kosten (nur aus RS1 gegen RS2 geschlossen, v6-RS1-Zeile unsauber);
  Stromnetz statt Akku; das Gefuehl des Menschen bei 8 fps (er hatte moeglicherweise andere Kamera/Skala).
- **Decisions taken:** Messung gegen Klon von Editor und Projekt (Original unberuehrt); Grossenklassen aus der glTF-Bounding-Box;
  Variante v5 uebernimmt v1 nur fuer kleine Pflanzen, v2 fuer die uebrigen; Pass-p50 bei RenderScale 2 als Hauptzahl, FPS nur Beleg.
- **Decisions suggested, not taken:** Ziel-Budget fuer den Menschen als Zahl: Hauptpass **unter ~6 Mio Dreiecke** (v5-Stand, 6,3 M) und
  Schatten+SSAO **unter ~2 ms** (v5: 0,8 + 1,0) ist mit handgemachten Mitteln erreichbar, kostet aber Optik (Abschnitt 6). Fuer 60 fps
  (16,7 ms) reicht es nicht: bei RenderScale 2 liegt schon der Boden (20,9 ms) darueber, bei RenderScale 1 liegt v5 bei 20,8 ms, also ~4 ms
  ueber der Grenze (Boden 16,8 ms, Tonemap-Zeuge dort unsauber). Die Restkosten sind Pixelkosten, das ist Aufgabe von Schritt 5.
- **Pending:** Schritte 2 bis 7 des Themas.
- **Traps:** siehe Abschnitt 7; Akku und Fremdlast; 60-Hz-Raster im FPS-Modus; `--scene` ueberschreibt dauerhaft.
- **Next steps:** 1. (vor Schritt 5) Klaeren, ob der Hauptpass ueberzeichnungs- oder eckpunktgebunden ist (z. B. Variante mit Pflanzenmaterial ohne
  Alpha-Test oder eine Xcode-GPU-Aufnahme), Pass-p50 bei RS 2; sonst spart Schritt 5 an der falschen Stelle. 2. Schritt 2 (meshoptimizer) mit den Zahlen aus 4.2 als Vorher; die Tris-Zeile je Pass braucht dafuer erst einen
  Zaehler in Schatten/SSAO (kleine Engine-Aenderung, nicht in diesem Schritt). 3. Schritt 4: Impostor-Material mit Pflanzenbild, sonst ist
  die Kreuzkarte optisch unbrauchbar. 4. Messung wiederholen am Netzstrom und bei geringer Last; Rohdaten-Skript `perf-raw-2026-10-29/cap.sh`.
