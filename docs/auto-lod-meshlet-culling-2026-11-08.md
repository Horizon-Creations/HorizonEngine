# Auto-LOD: Cluster-/Meshlet-Culling, Prototyp und Entscheidung (Thema 331, Schritt 6)

Stand 08.11.2026. **Entscheidung: Meshlet-Culling lohnt sich fuer Foliage nicht; LOD (Schritte 3 bis 5) plus
Instanz-/Bucket-Culling reichen.** Es wird kein Meshlet-Pfad im Renderer gebaut. Der Prototyp bleibt als Mess- und
Testwerkzeug (`MeshletCull.{h,cpp}`), damit die Frage mit anderen Meshes oder neuer Hardware in Minuten neu gestellt werden kann.

## 1. Was gebaut wurde (nur Werkzeug/Test, der Renderer ruft nichts davon)

* `src/HE_Tools/src/MeshSimplify/MeshletCull.{h,cpp}` (in `HorizonImporters`, meshoptimizer ist dort schon PRIVATE gelinkt):
  `build()` zerlegt ein `StaticMeshAsset` in Meshlets (64 Vertices / 124 Dreiecke, `meshopt_buildMeshlets`) mit Huellkugel,
  Normalenkegel und umsortiertem Indexpuffer; `Frustum::fromViewProj`, `meshletVisible` (Kugel gegen sechs Ebenen),
  `meshletBackfacing` (Kegel, **nur einseitige Flaechen**).
* `tests/test_meshlet_cull.cpp`: 4 Fälle gegen Brute Force (jedes Dreieck in genau einem Meshlet und in dessen Kugel; ein
  frustum-gecullter Meshlet hat keinen Eckpunkt im Frustum, 60 zufaellige Kameras; ein rueckseitiger Kegel heisst, dass
  jedes Dreieck der Kugel weg zeigt; leeres Mesh) plus die Sonde (Abschnitt 2), nur mit `HE_MESHLET_PROBE=1` und dem Testprojekt.

## 2. Messung: Foliage-Testszene, 12 Pflanzen, LOD0 (schlechtester Fall fuer die Entscheidung)

Sonde in `tests/test_meshlet_cull.cpp`: baut die Szene `docs/perf-audit/scenes/foliage-test-base.hescene` nach
(12 Terrains zu 12 m, Dichte x 144 m2 = 1901 Pflanzen, Scale 0,8..1,3, `drawDistance` je Layer, gleichverteilt, Seed 42; die
Platzierung ist **nicht** die der Engine, nur gleiche Zahl und Verteilung), nimmt die **echten** Meshes der Pflanzen
(`~/HorizonEngineTestAssets`, 6 232 bis 67 152 Dreiecke) und fragt: von den Pflanzen, die Reichweite und Instanz-Frustum
**schon heute** bestehen, was wuerde ein Meshlet-Test noch entfernen? Rohausgabe: `docs/perf-audit/meshlet-probe-2026-11-08.txt`.
Kamera A ist die Baseline-Kamera (0, 2,5, -24, 60 Grad, 2840x1528; Blickrichtung rekonstruiert, deshalb sehen 1531 statt aller
~1900 Pflanzen den Frustum; die Engine-Baseline hatte 47,75 M Dreiecke, hier 35,4 M).

| Kamera | Pflanzen im Frustum (Instanz-Test) | davon nur teilweise im Bild | Dreiecke nach Instanz-Test | nach Meshlet-Frustum | + Kegel (obere Schranke, einseitig) | Draws bei Bereichs-Zeichnung | CPU, 1 Thread |
|---|---|---|---|---|---|---|---|
| A Rand, ueber das Feld | 1531 | 276 (18 %) | 35,38 M | 31,58 M (**-10,7 %**) | 30,65 M (-13,4 %) | 1774 statt 1531 (+16 %) | 21,2 ms |
| B im Feld, 1,7 m | 534 | 111 (21 %) | 17,47 M | 15,48 M (**-11,4 %**) | 14,78 M (-15,4 %) | 618 statt 534 | 15,0 ms |
| C im Feld, seitwaerts | 670 | 115 (17 %) | 18,96 M | 17,60 M (**-7,2 %**) | 17,55 M (-7,5 %) | 777 statt 670 | 13,6 ms |

(Die CPU-Zeit enthaelt die Kegel-Huellen, also etwa das Doppelte des reinen Frustum-Tests; die Teilmenge 514 702 Meshlet-Tests
bei Kamera A.)

Was die Zahlen sagen:

1. **Nur Pflanzen am Bildrand profitieren.** Pflanzen, die ganz im Bild sind, verlieren nichts (A: 1255 von 1531). Meshlet-Culling
   ersetzt hier im Kern den Rand-Test, den das Instanz-Culling schon macht, durch einen feineren: 7 bis 11 % der Dreiecke.
2. **Der Normalenkegel bringt fuer Blattwerk fast nichts** (A: weitere 2,6 Punkte als *obere Schranke*, weil sie einseitige
   Flaechen annimmt). Pflanzenmaterial ist Cutout und doppelseitig: dort ist der Kegel-Test **ungueltig** (jedes Blatt ist von
   beiden Seiten sichtbar), der Gewinn faellt also ganz weg. Meshlets haben hier im Mittel 55 bis 88 statt 124 Dreiecke (Blattkarten
   haengen nicht zusammen), die Kegel sind breit.
3. **Okklusion kann der Prototyp nicht belegen und lohnt hier nicht zu raten.** Die Testszene ist flach (`heightScale 0`), Blattwerk
   ist Cutout und kann kein Occluder sein; die einzigen Occluder waeren Gelaende und Gebaeude, und die kennt das Instanz-/Bucket-Culling
   grob schon (`OcclusionCuller`).

## 3. Was es kosten wuerde, und was es bringen kann

* **Gewinn begrenzt durch die Pixel.** Baseline-Doc, Abschnitt 1: minus 87 % Dreiecke brachten nur minus 18 % Scene-Zeit
  (73,6 -> 60,6 ms); der Hauptpass haengt an Pixeln (Cutout-Ueberzeichnung). Linear umgerechnet bringen -11 % Dreiecke hoechstens
  ~2 % Scene-Zeit (LOD0, ~1,5 ms von 74) und **nach** den LOD-Ketten (6,3 M statt 47,75 M Dreiecke) etwa 0,2 ms. Das ist eine
  Schaetzung aus zwei Messpunkten, keine Messung des Meshlet-Pfads.
* **Kein Backend kann es billig.** Meshlet-Culling braucht GPU-Culling mit Indirect Draw (oder Mesh Shader). Laut Plan 2.3
  (Schritt-5-Handoff) hat **keines der fünf Backends** Indirect Draw/Compute-Culling im Pfad, macOS-OpenGL ist 4.1 ohne Compute.
  Der CPU-Weg schlaegt fehl: 13 bis 21 ms Einzelthread-Culling pro Frame (Tabelle) sind mehr als alles, was er spart, und
  er zerschneidet die Instanz-Batches in Bereichs-Draws (+15 % Draws, Instancing weg fuer jede angeschnittene Pflanze).
* **Speicher/Pipeline:** ein umsortierter Indexpuffer je LOD-Stufe jeder Pflanze, Meshlet-Tabelle (~48 Byte je Meshlet),
  zweite Zeichenroute in allen Backends, Schatten-/Velocity-/GI-/SSAO-Pfade muessten sie alle mitmachen (Cluster-Guards,
  siehe Plan 11).
* **Dagegen steht, was schon da ist:** Bucket-Culling (32 m), Instanz-Culling, Reichweite, Stufenwahl je Pflanze nach
  Bildschirmgroesse (Schritt 5), Impostor/Kreuzkarte als letzte Stufe (Schritt 4). Die grossen Hebel sind laut Baseline
  Schatten/AO (-38 ms), LOD (-51 ms) und Pixelkosten.

## 4. Wann die Frage neu zu stellen ist

* Fuer **grosse Einzelmeshes** (Gebaeude, Felsen-Cluster, Terrain-Chunks mit Millionen Dreiecken), die oft zur Haelfte hinter dem
  Betrachter oder verdeckt sind: dort sind Meshlets pro Objekt gross gegen das Objekt, und Kegel (einseitige Flaechen) und
  Hi-Z-Okklusion greifen. Das ist ein Thema fuer sich, nicht Foliage.
* Wenn ein Backend GPU-getriebenes Zeichnen bekommt (Metal Indirect Command Buffer/Object+Mesh Shader, Vulkan/D3D12 Mesh Shader):
  dann dort zuerst, hinter denselben Meshlet-Daten (`MeshletCull::build`).
* Wenn die Hauptpass-Kosten nach Schritt 5 wirklich eckpunktgebunden sind: erst `--detailed` mit RenderScale 1 und 2 auf der
  Szene mit gebackenen Ketten messen (offen aus Schritt 5), bevor Culling-Arbeit.

## 5. Nicht geprueft

* Kein Lauf im Editor/auf der GPU; die Sonde ist reine CPU-Rechnung auf nachgebauter Platzierung. Die Kamerablickrichtung der
  Baseline ist rekonstruiert. Keine Messung mit gebackenen LOD-Ketten (dort sind die absoluten Dreiecke 87 % kleiner; der Anteil
  am Bildrand aenderte sich dadurch nicht, die Kostenschaetzung ist daher in Abschnitt 3 proportional gekuerzt).
* Okklusion (Hi-Z) nicht prototypisiert, nur begruendet (Abschnitt 2, Punkt 3).
* Gesamt-`he_tests`: siehe Handoff.

## Handoff

* **Topic / step:** 331, step 6 (cluster/meshlet culling, prototype and decision).
* **Done:** `MeshletCull.{h,cpp}` (+ CMake in `src/HE_Tools/CMakeLists.txt`), `tests/test_meshlet_cull.cpp` (4 cases + opt-in probe, listed in
  `tests/CMakeLists.txt`), this doc, raw probe output `docs/perf-audit/meshlet-probe-2026-11-08.txt`.
* **Decision taken:** do NOT build meshlet culling for foliage (numbers in section 2/3). The prototype stays as tool/test code only.
* **Verified:** Release build of `he_tests`; `*test_meshlet_cull*` 5 cases / 194 771 assertions green (with and without the probe);
  probe run against the real plants of `~/HorizonEngineTestAssets/foliage-test-project`.
* **Not verified:** GPU/editor run, Hi-Z occlusion, chains baked into the scene, D3D/Vulkan (not built here).
* **Pitfall:** the probe's scatter is not the engine's (same count and distribution); camera direction reconstructed, hence 1531 and not
  ~1900 visible plants. Use the table for ratios, not as an absolute triangle budget.
* **Next steps:** 1. levelled witness / perf on the Foliage-Testszene (step 5 handoff); 2. step 7 of the topic; 3. revisit only on the
  conditions of section 4.
