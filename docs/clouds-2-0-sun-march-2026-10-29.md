# Wolken 2.0, Schritt 4d: Sonnenmarsch billiger (Thema 227, Hive-Schritt 1140, vierter Teil)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `f4d55830`. Gilt für `applyClouds3DReal` (3D, Realistic) in `SkyShaderSource.h` und im Metal-Spiegel.

## Was geändert ist
* **Medium marschiert die Sonne in 3 statt 4 Schritten** (`qShadow` 3) und reicht mit schnellerem Wachstum gleich weit (`stGrow` 2,6 statt 1,85: Abstände 15, 54, 155
  gegen 15, 43, 92, 175 Einheiten). Nur bei eingeschalteten Inter-Schatten (`uCloudInterShadows`), sonst bleibt "eigener Körper" wie vorher. Low und High unverändert.
* **Licht-Kadenz entfernt** (die aus Schritt 4c): sie war gemessen billig, aber sichtbar schlechter. Ein Cache, der an einem pro Pixel verschobenen Schritt beginnt (Blue-Noise-Sprung),
  macht Rauschen: mittlere Abweichung zum Original 3,3 von 255 gegen 1,3 ohne Cache. Eine Variante mit Schrittzahl-Kadenz zeigte horizontale Schichtung.
* **Wetterkarten-Interpolation bleibt, aber nur wo sie stimmt**: die Stützstellen sind seitlich höchstens 35 Einheiten auseinander (`qColEvery = min(.., 35 / (ds * |dir.xz|))`).
  Die erste Fassung (unbegrenzt) erzeugte horizontale Streifen an flachen Strahlen; jetzt gilt sie praktisch nur für steile Blicke (geprüft: Pitch 65 sauber). Bei Pitch 12 und 22 ist sie aus.
* Low und High sind **bitgleich zum Stand vor Schritt 4c** (Konstanten dort ohne Wirkung), Medium weicht im Mittel 1,3 von 255 ab (p99 10).

## Gemessen (Metal, M5, 2840x1528, `scripts/perf/sky_cloud_cost.py --repeat 5`, Scene-Pass p50, min / Median, Streuung unter 2 %)
| Variante | Anfang (a60e2430) | Zwischenstand 4c | jetzt | Änderung seit Anfang |
|---|---|---|---|---|
| ohne Wolken | 1,30 | 1,30 | 1,29 | |
| Low | 16,8 / 17,0 | 15,1 / 15,2 | 15,7 / 15,8 | -7 % |
| **Medium** | **28,0 / 29,2** | 25,6 / 25,6 | **20,9 / 20,9** | **-25 bis -28 %** |
| High | 37,3 (ein Lauf) | | 33,1 / 33,2 | -11 % (Bild bitgleich, siehe unten) |
| Medium + Low-Res | 12,9 / 13,1 | 13,0 / 13,1 | 7,9 / 7,9 | -39 % |
Wolken allein bei Medium: 19,6 ms (vorher ~27). Das ist immer noch mehr als der Spielraum eines 60-fps-Bildes (16,7 ms) bei nativer Auflösung, aber mit Low-Res 6,6 ms.
High: die Bilder sind bitgleich; die Zeit ist etwas niedriger, vermutlich weil der Compiler die geteilten Funktionen besser verarbeitet. Nicht untersucht.

## Verifiziert
Metal + OpenGL, `HE_SKY_TIME` fest: die zehn Bilder ohne Realistic-Wolken bitgleich zur Baseline; Realistic Low und High bitgleich zum Stand vor 4c, Medium 0,5 bis 1,3 von 255 Abweichung;
Metal gegen OpenGL Medium 0,8 von 255 (wie vor dem Umbau). Test: `test_culling` "weather-map cadence ... same numbers in GL and MSL" (High = 1). `ctest` voll 345/345 (privates HOME).

## Nicht geprüft
* D3D11/D3D12/Vulkan zur Laufzeit; die Alt-Fallback-Shader haben das Wolkenmodell nicht.
* Steile Strahlen mit Wetterkarten-Interpolation nur an einem Bild (Pitch 65) geprüft, nicht gegen eine Messung der Abweichung zum Original.
* Dämmerung, Classic und Dome nicht neu gemessen (nicht berührt).

## Handoff
* Wie weiter bei den Kosten: Medium + Low-Res ist jetzt 7,9 ms; Medium ohne Low-Res 20,9 ms. Weitere Kandidaten für das Licht: erste kurze Sonnenschritte ohne Wetterkarten-Neuberechnung
  (die Spalte der Wolkenprobe wiederverwenden), ein leichteres Licht-Feld, Low 2 Sonnenschritte. Jeweils mit `sky_cloud_cost.py --repeat 5` messen und Bild gegen den Stand prüfen.
* Feindetail und adaptive Schrittweite sind noch offen (Doc `clouds-2-0-cost-and-fine-detail-2026-10-29.md`); der Sonnenmarsch ist nach wie vor die Hälfte der Kosten.
* Lehre: ein Zwischenspeicher, der von der Position des Pixels abhängt (hier: Schritt mit Sprung-Offset), erzeugt Rauschen; Kosten zuerst an der Stelle senken, die nicht pro Pixel streut.
