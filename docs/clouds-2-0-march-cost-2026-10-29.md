# Wolken 2.0, Schritt 4c: Kosten des Realistic-Marschs senken, Low-Res geprüft (Thema 227, Hive-Schritt 1140, dritter Teil)

> **Korrektur (spätere Messung, siehe `docs/clouds-2-0-sun-march-2026-10-29.md`):** Die hier beschriebene Licht-Kadenz (`qLightEvery`) ist wieder entfernt: ihr
> Fehler ist Rauschen (Mittel 3,3 von 255 gegen 1,3 ohne), weil der Cache an einem pro Pixel verschobenen Schritt beginnt. Die Wetterkarten-Interpolation gilt nur noch
> für steile Strahlen (Abstand der Stützstellen auf 35 Einheiten seitlich begrenzt), ohne die Bedingung zeigte sie horizontale Streifen am Horizont.
> Die Kostentabelle unten ist der Zwischenstand.


Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `a60e2430`. Gilt für `applyClouds3DReal` (3D, Realistic) in `SkyShaderSource.h` und im Metal-Spiegel.

## Was geändert ist
* **Licht-Kadenz** (`qLightEvery`: Low 3, Medium 2, High 1): der Sonnenmarsch läuft nur auf jedem n-ten Wolkenschritt, dazwischen wird sein Ergebnis wiederverwendet (`sunCache`);
  ein leerer Schritt vergisst den Cache. Dichte und Deckkraft werden weiter an jedem Schritt berechnet.
* **Wetterkarte nur an jedem K-ten Schritt** (`qColEvery`: Low 4, Medium 3, High 1) und dazwischen interpoliert. Dafür ist `cloudFieldDensity` in
  `cloudColumnReal` (Wetterkarte: cover, macro) und `cloudBodyReal` (3D-Teil) geteilt; `cloudFieldDensity` ruft beide hintereinander auf,
  Sonnenmarsch und Wolkenschatten-Karte rechnen also unverändert exakt.
* **High bleibt exakt**: beide Konstanten sind dort 1, die Bilder sind bitgleich zum vorherigen Stand (Test: `test_culling` "cadences ... same numbers in GL and MSL", High = 1).

## Gemessen (Metal, M5, 2840x1528, `scripts/perf/sky_cloud_cost.py --repeat 5`, Scene-Pass p50 in ms, min / Median von 5 Läufen)
| Variante | vorher (a60e2430) | nachher | Änderung |
|---|---|---|---|
| ohne Wolken | 1,30 | 1,30 | |
| Low | 16,8 / 17,0 | 15,1 / 15,2 | -10 % |
| **Medium** | **28,0 / 29,2** | **25,6 / 25,6** | **-9 bis -12 %** |
| Medium + Low-Res | 12,9 / 13,1 | 13,0 / 13,1 | 0 |
Die Läufe sind jetzt eng (Streuung unter 3 %); die frühere Streuung kam von Fremdlast auf der GPU (WindowServer, andere Apps: Device Utilization 15-90 %).

## Wo die Zeit wirklich sitzt (Experimente, nur zum Messen, nicht im Code)
* **Sonnenmarsch ohne Schritte** (`qShadow = 0`) bei sonst gleichem Medium: **12,8 ms** statt 25,6. Der Sonnenmarsch ist also **die Hälfte** der Kosten, auch mit Kadenz 2.
* Fast leere Schicht (Coverage 0,05): 12,7 ms, Medium mit einem Sonnenschritt weniger (`cloudInterShadows` aus, 4 -> 3): -3,2 ms.
* Mein erstes Kostenmodell war falsch (Wetterkarte "fast die Hälfte", Kadenz "die Millisekunden"): beides zusammen brachte nur 10 %. Der Sonnenmarsch bleibt der Hebel.

## Low-Res als Standard geprüft (nicht umgestellt)
* Statisch fast gleich: Vollbild gegen Low-Res mittlere Abweichung 0,69 von 255 (p99 7), auf Metal und OpenGL gleich. Kosten Medium + Low-Res 13,0 gegen 25,6 ms.
* Nicht geprüft, weil nicht headless messbar: Verhalten beim Schwenken (Reprojektion der Vorderkamera, 1 Frame Verzögerung). Das braucht einen Blick im Editor
  (Checkbox "Low-res clouds" im Environment-Inspector).
* Gegen eine Umstellung des Standards: Vulkan/D3D11/D3D12 marschieren die Wolken immer inline (das Prelude setzt `uLowResClouds`/`uCloudPrepass` auf 0): derselbe Standard hätte dort
  volle Kosten und ein anderes Bild. Der Serializer schreibt alle Umgebungsfelder (`HE_ENV_SER_PLAIN`), betroffen wären nur Szenen ohne den Schlüssel und neue Himmel.
  Empfehlung: Standard so lassen und Low-Res als bewusste Option anbieten, die Entscheidung liegt beim Menschen.

## Nicht gebaut (und warum)
* **Adaptive Schrittweite** (grob durch leeren Raum, fein nach dem ersten Treffer) und das **erneute Anwenden des Feindetails** als Beweis: nicht gebaut. Feine Schritte erhöhen
  die Zahl der Dichteauswertungen, und der Sonnenmarsch (die Hälfte der Kosten) wäre nicht gesenkt. Erst die Kosten des Lichts, dann Auflösung.

## Verifiziert
Metal + OpenGL, `HE_SKY_TIME` fest: die zehn Bilder ohne Realistic-Wolken bitgleich zur Baseline; Realistic High bitgleich zum vorherigen Commit, Medium mittlere Abweichung 3,0 von 255 (p99 29, Rand),
Low 6,5. Metal gegen OpenGL Medium 1,5 von 255 (vorher 0,8), Low 3,6 (vorher 2,2). `ctest` voll 345/345 (privates HOME).

## Handoff
* Nächster Schritt: den **Sonnenmarsch billiger machen**. Kandidaten (jeweils mit `sky_cloud_cost.py --repeat 5` messen und Bild gegen den Stand prüfen):
  Kadenz 3 und 3 statt 4 Sonnenschritte bei Medium (erwartet ~-6 ms), den Sonnenmarsch ohne Wetterkarten-Neuberechnung für die ersten kurzen Schritte, ein leichteres Licht-Feld.
  Danach Feindetail (siehe `docs/clouds-2-0-cost-and-fine-detail-2026-10-29.md`).
* Entscheidung beim Menschen: Low-Res als Standard (siehe oben).
