# Auto-LOD: Impostor-Ueberzeichnung senken (Thema 331, Schritt 11 / Plan-Nr. 14)

Stand 08.11.2026, Metal auf dem M5 (MacBook Air), Netzbetrieb, Bildschirm gesperrt, Standard-RenderScale (Fenster 2840x1528),
Kamera `--cam 0,2.5,-24,3.14159,-0.15`, `HE_SKY_TIME=40000`, Szene Kette + Impostor (12 Layer). Baut auf
`docs/auto-lod-gpu-capture-stutter-2026-11-08.md` (Schritt 10) und `docs/auto-lod-impostor-2026-11-08.md` (Schritt 4).
Rohdaten und Skripte: `docs/perf-audit/auto-lod-impostor-overdraw-raw-2026-11-08/`, Bilder `docs/perf-audit/img/auto-lod-impostor-overdraw-*.png`.

## 1. Ergebnis in vier Saetzen

* **Die Impostor-Karten sind zu 80 bis 98 % durchsichtig** (undurchsichtiger Anteil der Karten-Flaeche: 1 bis 22 %, Mittel ca. 8 %), und **ein verworfenes Fragment kostet hier fast so viel wie ein beleuchtetes**: alle Karten-Fragmente per Cutoff 2,0 verworfen = gleiche Scene-Zeit wie normal (13,2/13,6 gegen 13,3/13,0 ms); Karten auf 2 % geschrumpft (fast keine Fragmente) = Scene -1,6 ms. Es ist also die *Flaeche der Karte*, nicht Beleuchtung, nicht Doppelseitigkeit, nicht Alpha-Mischung.
* **Wirksamste Massnahme: die Karte pro Ansicht auf die Alpha-Silhouette stutzen** (vier Baender, je hoechstens zwei Laeufe, gleiche Nachbarn verbunden: ca. 4 Vierecke je Ebene statt einem, ca. 40 % der Rechteckflaeche). Reine Bake-Aenderung (`Impostor::cardRects` / `makeCard(Baked)`), identisch auf allen Backends, kein Renderer-Code.
* **Metal System Trace (echte GPU-Zeit je Encoder): Scene-Encoder 5,7 -> 2,5 ms, Summe aller Encoder 14,7 / 14,9 -> 11,7 / 11,95 ms (-3 ms, -20 %)**; der Impostor-Anteil (Summe minus Boden+Wolken ohne Pflanzen, 11,03 ms) faellt von 3,7 auf ca. 0,7 bis 0,9 ms.
* **Optisch keine Verschlechterung, eher naeher an der Geometrie:** die alten Rechteck-Karten warfen rechteckige Schattenflecken und liessen die Felder dunkler als die echten Pflanzen aussehen; die gestutzten Karten sind heller gruen wie die Geometrie (Abschnitt 5).

## 2. Die drei Massnahmen der Aufgabe, was jede ergab

| Massnahme | Befund |
|---|---|
| Alpha-Test-Schaerfe / Dither statt weichem Alpha-Blend | **Entfaellt: es gibt keinen weichen Blend.** Das Impostor-Material ist `Masked` (Cutoff 0,2, harter Discard seit Schritt 4, Discard vor allen anderen Samples seit Schritt 8). |
| Engere Karten-Geometrie (weniger transparente Randflaeche) | **Wirkt, umgesetzt** (Abschnitt 3). |
| Early-Z / Depth-Prepass fuer die Impostor-Schicht | **Nicht gebaut.** Der Versuch "alles verwerfen" zeigt, dass die Beleuchtung der ueberlebenden Fragmente nichts Messbares kostet; ein Prepass haette nur diesen Teil gespart (und braucht eine neue Shader-Variante je Backend = Pipeline-Architektur, ausserhalb des Schritts). |
| Zusaetzlich geprueft: einseitige Karte | Alle Szenen-Passes laufen mit Cull None (Metal/D3D/Vulkan/GL), die Karte traegt beide Seiten selbst. Einseitig: Scene 13,3 -> ca. 13,1 ms (Rauschen), Dreiecke 140k -> 133k. **Nichts gewonnen**, und der Schattenpass culled Vorderseiten (Cull Front), eine einseitige Karte wuerfe nur bei abgewandter Seite Schatten. Bleibt doppelseitig. |

## 3. Was gebaut ist

`src/HE_Tools/src/MeshSimplify/Impostor.{h,cpp}`:

* `cardRects(baked, view)`: je Ansicht vier waagerechte Baender; je Band die Texel, die den Cutoff ueberhaupt bestehen koennen (Alpha >= 0,1 = halber Cutoff: eine bilineare Probe ist ein konvexes Gemisch, sie besteht nur neben einem Texel ueber dem Cutoff), seitlich und senkrecht um `max(2, size/64)` Texel (8 bei 512) verbreitert (Filter, niedrige Mips), Luecken unter einem Viertel der Breite ueberbrueckt, hoechstens zwei Laeufe je Band, Laeufe gleichen Umfangs im Nachbarband zu einem hoeheren Rechteck vereint. Leere/ungueltige Ansicht = ein Rechteck ueber alles (altes Verhalten).
* `makeCard(const Baked&, doubleFaced = true)`: pro Rechteck ein Viereck, beide Seiten, Position und UV laufen durch dasselbe Texel (v = Zeile/size, Achse = (x/size)*2-1 mal Halbbreite); Pivot, Normalen (zum Himmel geneigt) und Wicklung wie zuvor. `makeCard(halfWidth, y0, y1, doubleFaced)` (ein Rechteck) bleibt.
* `write()` nutzt die gestutzte Karte. **Dateinamen, UUIDs, Material, Textur, Szenenformat unveraendert**: eine Szene mit alter Karte laedt weiter, ein erneutes Backen ersetzt die Karte an Ort und Stelle (die bestehende Karte `*_ImpostorCard.hasset` wird einfach umgeschrieben).

Kosten: Karten haben jetzt im Mittel 32 statt 8 Dreiecke (Hauptpass 140k -> 181k Dreiecke). Schatten- und SSAO-Pass zeichnen die Karten mit und zahlen dafuer (Schatten 0,88 -> 1,22, SSAO 0,43 -> 0,56 ms in den Zaehlern), das ist im Trace-Ergebnis unten bereits enthalten.

### Wahl der Parameter (gleiche Szene, Zaehler `--detailed`, Mittel der Laeufe, ms; GPU = `gpuMs_median`)

| Variante | Baender x Laeufe | Flaeche (Mittel ueber 24 Ebenen) | Dreiecke Hauptpass | Scene | Schatten | SSAO | GPU |
|---|---|---|---|---|---|---|---|
| A: Rechteck (Schritt 4) | - | 100 % | 139 924 | 13,17 | 0,89 | 0,43 | 15,44 |
| T | 8 x 3 | 34 % | 252 920 | 12,39 | 1,34 | 0,66 | 15,58 |
| **V (gebaut)** | **4 x 2** | **40 %** | **180 576** | **12,19** | **1,22** | **0,56** | **15,01** |
| W | 2 x 2 | 46 % | 153 588 | 12,37 | 1,11 | 0,51 | 15,12 |
| X: V einseitig | 4 x 2 | 40 % | 153 418 | 12,37 | 1,17 | 0,51 | 15,29 |

Mehr Baender sparen kaum Flaeche und kosten Dreiecke (T ist ueber die Zaehler schlechter als A); zwei sind zu grob. Die Zaehler-GPU-Zeit zeigt nur -0,4 ms, weil sie die Wolken im Scene-Pass mitzaehlen und der GPU-Takt bei weniger Last sinkt (Schritt 10, Abschnitt 3.2); **belastbar ist der Trace**.

## 4. Vorher/Nachher-GPU-Zeit im Metal System Trace (p50 ueber ca. 260 Frames, ms, Encoder wie in Schritt 10)

| Lauf | Scene-Encoder (Enc 8) V + F | Summe aller Encoder |
|---|---|---|
| A (Rechteck) Lauf 1 | 0,27 + 5,54 | **14,67** |
| A Lauf 2 | 0,18 + 5,72 | **14,89** |
| **V (gebaut)** Lauf 1 | 0,57 + 2,01 | **11,95** |
| **V** Lauf 2 | 0,39 + 2,01 | **11,69** |
| T (8 x 3) Lauf 1 / 2 | 0,58 + 1,87 / 0,55 + 1,84 | 11,86 / 11,67 |
| W (2 x 2) Lauf 1 / 2 | 0,19 + 2,37 / 0,19 + 2,43 | 11,56 / 11,83 |
| Referenz Schritt 10: Boden + Wolken, ohne Pflanzen | 0,73 (gesamt) | 11,03 |

* **Scene-Encoder -3,2 ms (5,8 -> 2,6), Summe -3,0 ms.** Die Vertex-Zeit des Scene-Encoders steigt um 0,2 bis 0,4 ms (mehr Dreiecke), das Fragment sinkt um 3,5 ms. Reihenfolge ABAB, Last am Anfang 2,8 bis 3,2.
* Impostor-Anteil nach Schritt-10-Definition (Summe minus Boden+Wolken): A 3,6 bis 3,9 ms -> V 0,7 bis 0,9 ms. Bei den Takt-Effekten aus Schritt 10 (der Trace ueberschaetzt, was Entfernen spart) ist die Grossenordnung belastbar, nicht die zweite Stelle.
* **Fehlschlag, ehrlich:** die ersten Zaehler-Laeufe (`--detailed`) zeigten nur -0,3 bis -1,2 ms Scene und kaum GPU-Gewinn, weil Schatten/SSAO mehr Dreiecke zeichnen und die Zaehler Wolken und Takt vermischen. Ohne den Trace waere die Wirkung unterschaetzt worden.

### 4.1 Durchsatz (fps) und GPU-Zeit je Frame, Standard-RS, ohne Zaehler (A/F abwechselnd, 4 Runden, Vsync aus)

| | GPU-Zeit je Frame p50 (Command Buffer) | Mittel-dt | fps (Mittel-dt) |
|---|---|---|---|
| A | 23,7 / 26,7 / 25,0 / 23,9 | 18,2 / 15,0 / 16,7 / 16,4 | 55 / 67 / 60 / 61 |
| F (V) | 14,5 / 13,2 / 14,3 / 15,2 | 15,6 / 16,9 / 16,8 / 16,8 | 64 / 59 / 60 / 60 |

* Die **GPU-Zeit je Frame faellt um ca. 10 ms (24 -> 14)**, der **Durchsatz bleibt bei ca. 60 fps in beiden**: auf dem gesperrten Bildschirm ist das Frame-Tempo vom Drawable-Warten bestimmt (Schritt 10, Abschnitt 4), nicht von der GPU. Die Impostor-Senkung hebt also die GPU-Reserve (mehr Luft fuer Wolken/Schatten/groessere Szenen), **nicht** die gemessenen fps.
* Vsync-Laeufe (`--vsync keep`) sind **unbrauchbar**: andere Bienen kompilierten (Load 11 bis 24), beide Seiten zeigten 10 bis 28 fps. Nicht in die Auswertung genommen, Rohdaten liegen bei (`*vs_fps_*`). Der Vsync-Stand aus Schritt 10 (53 fps Endkonfiguration) wurde **nicht** neu gemessen.

## 5. Gesehen (gleiche Kamera, Frame 300, `HE_CAPTURE_FRAME`, `HE_SKY_TIME=40000`)

* `docs/perf-audit/img/auto-lod-impostor-overdraw-A2.png` (vorher), `...-F.png` (nachher), `...-G.png` (Geometrie, Szene v0 ohne LODs, Frame 120: einzelne Boden-Kacheln dort noch ungeladen/weiss, das ist Streaming, nicht Pflanze).
* **Nachher ist heller gruen, die dunklen Flecken sind weg.** Die alten Karten warfen ihre ganze Rechteckflaeche als Schatten (der Schattenpass zeichnet Karten ohne Alpha-Cutout-Bezug zur Silhouette) und beschatteten sich gegenseitig; die gestutzten Karten werfen nur noch ihre Silhouette. Gegen die echte Geometrie (G) weichen 34,9 % der Pixel um mehr als 16 Stufen ab (nachher) gegen 39,9 % (vorher); Rauschniveau Lauf gegen Lauf desselben Zustands: 2,4 %. Es ist **keine Pixelgleichheit** und nicht gedacht; dass die Karten bei Schatten heller werden, ist eine sichtbare Aenderung, nach den Bildern naeher an der Geometrie.
* Nicht geprueft: Nahaufnahme einzelner Karten (Mip-/Filterrand), Sicht von der Seite auf sehr duenne Pflanzen. Die Randverbreiterung ist fuer Mips bis ca. Stufe 3 gedacht; weiter entfernt kann die Karte die Silhouette um hoechstens ca. ein Bildschirmpixel anschneiden.

## 6. Tests

* `he_tests`: `test_impostor` (jetzt 9 Faelle: neu "gestutzte Karte deckt jedes Texel ueber dem Cutoff und viel weniger als das Rechteck", "leere Ansicht/kein Atlas -> ganze Ansicht", "Position und UV laufen durch dasselbe Texel, beide Seiten entlang ihrer Normalen, einseitig = halbe Vierecke"), `test_foliage*`, `test_auto_lod`: `ctest -R "test_impostor|test_foliage|test_auto_lod"` 9/9 gruen (Build-Ordner `out/build/release`, der Ordner `cmake-build-release` aus der Aufgabe existiert in diesem Worktree nicht). Voller Lauf: siehe Handoff.

## 7. Nicht verifiziert

* Windows/Linux, D3D11/D3D12/Vulkan/GL (die Karte ist normale Szenendaten; Cull None und Cutoff-Verhalten sind im Quelltext aller Backends gleich gelesen, nicht gelaufen).
* Vsync-Durchsatz (Messlaeufe wegen Fremdlast verworfen), entsperrter Bildschirm.
* Per-Draw-Kosten im Scene-Encoder (kein Xcode-GUI-Capture); "verworfenes Fragment kostet so viel wie beleuchtetes" ist aus zwei Gegenproben (alles verworfen, Karte 2 %) geschlossen, nicht aus einem Shader-Profil. Warum der Schattenpass bei mehr Karten-Dreiecken nur schwach waechst (Schatten 0,88 -> 1,34 bei 9x Dreiecken, 1,22 bei 4x): nicht untersucht.
* Impostor-Schatten pro Alpha (Schattenpass mit Cutout) nicht geprueft.

## Handoff

- **Topic / step:** 331 Auto-LOD, Schritt 11 (Plan-Nr. 14): Impostor-Ueberzeichnung senken. Branch `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`.
- **Done:** Ursache gemessen (Karten 80-98 % transparent; Fragment-Anzahl, nicht Beleuchtung/Doppelseitigkeit), Karte pro Ansicht gestutzt (`cardRects`, `makeCard(Baked)`), Tests, Trace-Messung vorher/nachher, Bilder, Abnahme-Doku aktualisiert.
- **Result in one line:** Scene-Encoder 5,7 -> 2,5 ms, Encoder-Summe 14,7/14,9 -> 11,7/11,95 ms (Impostor-Anteil 3,7 -> ca. 0,8 ms), GPU je Frame 24 -> 14 ms, fps bleiben ca. 60 (Drawable-gebunden), Bild naeher an der Geometrie.
- **Files:** `src/HE_Tools/src/MeshSimplify/Impostor.{h,cpp}`, `tests/test_impostor.cpp`, dieses Doc, `docs/auto-lod-abnahme-2026-11-08.md` (Abschnitt 8), `docs/auto-lod-gpu-capture-stutter-2026-11-08.md` (Update-Zeile), Rohdaten `docs/perf-audit/auto-lod-impostor-overdraw-raw-2026-11-08/`, Bilder `docs/perf-audit/img/auto-lod-impostor-overdraw-{A2,F,G}.png`.
- **Verified:** s. Abschnitte 3 bis 6 (Trace ABAB, Zaehler-Varianten je 3 Runden, fps 4 Runden A/F, Bilder angesehen, 9 Tests gruen).
- **Not verified:** Abschnitt 7.
- **Decisions taken:** Stutzen statt Prepass/Dither/einseitig; Parameter 4 x 2, Luecke 1/4, Rand `size/64`; doppelseitig bleibt (Schatten-Cull-Front).
- **Decisions suggested, not taken:** Karten fuer Schatten-/SSAO-Pass vereinfachen (Dreieckskosten dort 0,3 bis 0,5 ms); Cutoff-bewusster Schatten; Wolken-Pre-Pass (6,5 ms) ist jetzt der groesste Einzelblock, eigenes Thema.
- **Traps:** (1) Die ersten Zaehler-Vergleiche zeigten fast nichts, der Trace -3 ms: fuer Impostor/Wolken immer den Trace nehmen. (2) `Impostor.cpp` steckt in der statischen `HorizonImporters`, die in `HorizonEditor` gelinkt ist: ein getauschtes `libHorizonTools.dylib` aendert nichts, der Editor muss neu gelinkt werden. (3) `HorizonSDKRelease` brach mit "dev MISMATCH": `out/build/release` und `out/build/release/sdk-release` trugen verschiedene `HE_GIT_COMMIT` in `HorizonVersion.h`; `cmake -S . -B out/build/release` und `cmake out/build/release/sdk-release` (Neu-Konfigurieren) behob es. (4) Cutoff-/Geometrie-Experimente per `getenv` im Bake waren temporaer und sind entfernt. (5) `he_perf_capture`-Aufnahmen `HE_CAPTURE_PATH` sind PPM trotz `.png`-Endung (`sips -s format png`).
- **Next steps:** 1. Queen: PR-Entscheidung mit Abschnitt 1/4 und dem 60-fps-Stand aus `docs/auto-lod-abnahme-2026-11-08.md` Abschnitt 8. 2. Optional: Wolken-Pre-Pass amortisieren (eigenes Thema). 3. Optional: Schattenpass-Karte vereinfachen. 4. CI-Ergebnis des neuen Heads abwarten.
