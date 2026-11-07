# Wolken 2.0, Schritt 4b: Kosten der Wolken gemessen, Feindetail-Versuch (Thema 227, Hive-Schritt 1140, zweiter Teil)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `96fd5ca8`. **Kein Shader geändert**: der Feindetail-Versuch ist zurückgenommen (siehe 2).
Neu: `scripts/perf/sky_cloud_cost.py` (reproduzierbare GPU-Messung der Wolken, baut sein Mini-Projekt selbst).

## 1. Was die Wolken kosten (Metal, M5, Retina-Fenster 2840x1528, Render-Skala 1, Release, he_perf_capture --detailed)
Himmel-only-Szene (`docs/perf-audit/scenes/skyonly.hescene`), Kamera am Boden, 22 Grad nach oben, 3D-Realistic-Schicht 300 m hoch, Coverage 0,55.
Scene-Pass-p50 in ms (ohne Wolken 1,3 ms):

| Variante | Scene p50 | Wolken allein | FPS ganze Szene (ungefähr) |
|---|---|---|---|
| ohne Wolken (Coverage 0) | 1,3 | 0 | 103 |
| Low (cloudQuality 0) | 17,1 | 15,8 | 56 |
| **Medium (Standard)** | **26,7** | **25,4** | **34** |
| High | 37,3 | 36,0 | 26 |
| Medium + Low-Res (Viertel-Auflösung) | 12,8 | 11,5 | 74 |

Rauschen: ein zweiter Lauf desselben Mediums gab 29,1 ms (+9 %), das ist die Messunsicherheit (Display, Fremdlast).
Das ist **der** Hebel für die Bildrate in jeder Szene mit sichtbarem Himmel: bei nativer Auflösung braucht Medium allein mehr als ein 60-fps-Bild (16,7 ms).
Das erklärt einen Teil der niedrigen fps in der Foliage-Testszene mit offenem Himmel, ist aber nicht gemessen dort.

## 2. Feindetail-Versuch (zurückgenommen)
In `applyClouds3DReal` zwei weitere Worley-Oktaven (curl-verzerrt, ein Fetch für die Verzerrung), nur nah an der Kamera und nur in der dünnen Haut
einer Wolke, 3 Fetches pro betroffenem Schritt (~6 % der Fetches eines Schritts). Ergebnis auf Metal:
* Die Wolken sehen praktisch gleich aus, auch aus nächster Nähe (Kamera 20-150 m unter der Basis). Eine extrem harte Testmaske (Schritt 0,45..0,55 statt der Erosion)
  macht die Wolke nur durchsichtiger und körnig, sie bringt keine Lappen hervor.
* Ursache: **die Schrittweite**. Realistic Medium marschiert `N = clamp(Strecke / (0,28 * 300), 26, 80)` Schritte, also ~12 bis 80 Welteinheiten pro Schritt.
  Eine Oktave mit Zellen von 20 bis 50 Einheiten ist feiner als der Abstand der Proben und wird vom Sprung-Offset nur zu Körnung. Die Noise-Zelle der Feinoktave
  `np*2.6` (255 Einheiten) liegt dagegen weit über der Auflösung, deshalb sind die Wolken weich.
Mehr Detail im Shader bringt ohne den Marsch umzubauen also nichts; es kostet nur Fetches.

## 3. Konsequenz und Plan (Reihenfolge)
1. **Kosten zuerst.** Ein Marsch mit adaptiver Schrittweite: grob (jetzt) durch leeren Raum und die Haut, ab dem ersten Treffer mit einem Drittel der Schrittweite
   weiter, bis die Transmission fällt. Das spart in leeren und dichten Bereichen und löst zugleich die Körnung (Strahlen treffen die Oberfläche genauer)
   und schafft erst die Auflösung, in der Feindetail sichtbar wird. Messen mit `scripts/perf/sky_cloud_cost.py` (Ziel: Medium unter 12 ms bei Retina).
2. **Low-Res als Standard prüfen** (11,5 ms statt 25,4 ms): nur Metal und OpenGL, 1 Frame Verzögerung, Reprojektionsartefakte beim Schwenk.
3. **Dann Feindetail** (diesen Versuch mit den Konstanten dieses Dokuments wieder anwenden: `np * 13`, Haut-Maske, `detailNear` 2..10 % von `maxDist`),
   erst jetzt ist es sichtbar.
4. Wolkentypen, Horizont, Dome-Modus wie im Survey (`docs/clouds-2-0-survey-2026-10-29.md`).

## Nicht geprüft
* Classic-Stil und Dome-Modus nicht gemessen (`--style 0 --mode 0` des Skripts). GL, D3D und Vulkan nicht gemessen (nur Metal).
* Kosten auf der Foliage-Testszene nicht gemessen. Messung bei wachem Display und ohne Fremdlast, aber Device Utilization schwankte zwischen 5 und 30 %.

## Handoff
* Entscheidung beim Menschen (offen seit Schritt 4a): Low-Res als Standard und/oder Marsch-Umbau. Mein Vorschlag: Marsch-Umbau zuerst (hilft auf allen Backends), Low-Res-Standard danach.
* Wiederholbar: `scripts/perf/sky_cloud_cost.py --variants clear,q0,q1,q2,lowres` (Ausgabe: Scene-Pass-p50), danach vergleichen.
