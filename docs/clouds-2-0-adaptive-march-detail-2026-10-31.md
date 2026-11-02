# Wolken 2.0, Nachrunde: adaptiver Marsch, Feindichte, rundere Kronen (Thema 227, Folge zu Hive-Schritt 1140)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `e1a5f115` (plus Merge von `release/0.8.0`). Gilt für `applyClouds3DReal`, `cloudShapeParams` (3D, Realistic), GL-Quelle `SkyShaderSource.h` und Metal-Spiegel.
Bild (Metal, oben vorher, unten nachher: Boden 30 Grad, Boden 12 Grad, 100 m unter der Basis, Typ 1): `docs/clouds-2-0-survey/adaptive-march-detail-2026-10-31.jpg`.

## Warum und was
4b (`docs/clouds-2-0-cost-and-fine-detail-2026-10-29.md`) hat gezeigt: mehr Oktaven im gleichen Marsch bringen nichts, die Schrittweite ist gröber als die Zellen. Also zuerst der Marsch, dann die Dichte.
* **Adaptiver Marsch.** Grob (wie bisher) durch leeren Raum. Beim ersten Treffer (`dens > 0,01`, `t < 0,12 * maxDist`, Strahl noch zu mehr als 40 % klar) wird dieses Intervall mit
  `ds * qFineF` neu marschiert (Low 0,5, Medium 0,34, High 0,25), bis zwei leere Feinproben folgen oder der Strahl undurchsichtig wird (Innenraum braucht keine Auflösung). Die Position `tCur` ist explizit
  (Schrittweite wechselt), der Blue-Noise-Sprung einmal am Eintritt; Obergrenze `NM = 1,6 * N + 4` Schritte. Die Wetterkarte wird in Grobschritten wie vorher alle K Schritte interpoliert
  (Anker nach Abstand statt nach Index, der Anker wird weiterverwendet), in Feinschritten exakt gelesen.
* **Feindichte** nur im Feinmodus und nur in der Haut einer Wolke (`skin`, nah): zwei curl-verzerrte Worley-Oktaven (Zellen ~70 und ~35 Einheiten) tragen die Dichte ab wie die grobe Erosion.
* **Licht:** wo ein Feinsegment stattfand, sind die Oberflächen-Lumps (4e) stärker (1,6 statt 1,0, Klemme 0,4..1,8): bei den Referenzen kommt der Blumenkohl-Eindruck vor allem aus dem Licht.
* **Rundere Kronen:** `cloudShapeParams` Typ 0 deckelt die Turmhöhe jetzt bei 1,0 (vorher kein Deckel): die Wolken laufen nicht mehr in die flache Oberkante der Platte. Das ändert Typ 0 sichtbar (neue Baseline).
* Nebeneffekt, sichtbar im Bild: die **Körnung auf den Wolkenflächen ist weg** (sie war Schrittweiten-Aliasing durch den Sprung-Offset). Auch im Low-Res-Pfad (Standard im Editor).

## Geprüft (Mac, Release, `HE_SKY_TIME` 300, AA aus, 1280x720)
* Sieben Zeugen ohne Realistic (Dome 3x, Classic 3x, klar) **bitgleich** zum Stand vor der Runde. Realistic ist absichtlich anders (Mittel 0,7..2,3 von 255, Max 80..92).
* Metal gegen GL: im Mittel 0,46 bis 0,64 von 255 (vier Bilder, Max 50..80).
* `test_culling` erweitert (Feinschritt-Zahlen gleich in GL/MSL und fallend mit der Qualität, Feinmodus-Grenzen im Text beider Dialekte), `test_sky_shader`, ctest voll **350/350** (privates HOME).
* Versuche, die verworfen sind: Feinschritt 0,5 bei Medium (Körnung, Probenabstand größer als die kleinsten Zellen), Sonnenmarsch nur in jedem dritten Feinschritt mit Zwischenspeicher (Körnung: welcher Schritt marschiert hängt vom Pixel ab).

## NICHT geprüft: Kosten
**Keine gültige GPU-Messung.** Der Mensch hat während der Läufe den Stromsparmodus eingeschaltet; derselbe unveränderte Merge-Stand maß 42 statt 21,7 ms (Medium), also sind alle Zahlen dieser Runde wertlos
und hier nicht angegeben. Erwartung: der Feinmodus kostet pro Pixel mit Wolkenhaut zusätzlich ~3..6 Körper-Auswertungen samt Sonnenmarsch; der Spar-Teil (leerer Raum bleibt grob, Innenraum grob) gleicht davon nur einen Teil aus.
* **Zuerst messen** (`scripts/perf/sky_cloud_cost.py --variants clear,q0,q1,q2,lowres,type4 --repeat 5`, Netzbetrieb, Stromsparen aus, ruhige Maschine) gegen `e1a5f115` (Medium 21,7, High 34,7..35,9).
  Hebel, falls zu teuer, in dieser Reihenfolge: `qFineF` Medium 0,34 -> 0,40; `tFineMax` 0,12 -> 0,08; Feinoktave nur ab High; Sonnenmarsch im Feinmodus mit einem Schritt weniger (nicht zwischenspeichern: das gibt Körnung).
* D3D11/D3D12/Vulkan zur Laufzeit nicht geprüft (NN-WS03 offline); Dämmerung nicht eigens betrachtet.

## Offen gegenüber den Referenzen
* Es sind keine scharfen Blumenkohl-Wülste: die Lumps liegen im Licht und in einer kleinen Dichte-Erosion, die Silhouette bleibt weich. Nächster Hebel: Feindichte in der ersten Sonnen-Probe (Selbstschatten je Wulst) oder kleinere Feinschritte nur an der Silhouette.
* Cumulonimbus liest sich als Masse statt als Turm; der Himmel ist weniger gesättigt blau als die Fotos (Atmosphäre, nicht Wolken).

## Handoff
* Als Nächstes: Kosten messen (siehe oben), danach entscheiden, ob `qFineF` bleibt. Schritt 1141 (Windows) bleibt offen.
