# Wolken 2.0, Schritt 4e: Unterseiten und Oberflächen-Detail (Thema 227, Hive-Schritt 1140, fünfter Teil)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `8dfb8641`. Gilt für `applyClouds3DReal` und `cloudBodyReal` (3D, Realistic), GL-Quelle `SkyShaderSource.h` und Metal-Spiegel.
Bild links vorher, rechts nachher: `docs/clouds-2-0-survey/underside-detail-before-after-2026-10-29.jpg` (Metal; Boden 40 Grad, Boden 8 Grad, 100 m unter der Basis, knapp unter der Basis).

## Was geändert ist
* **Unterseiten nicht mehr abgeschnitten.** Vorher war der Boden eine Ebene bei `baseY` (Anstieg über 0,06 der Dicke, die Marschstrecke endete dort): eine glatte, gerade Kante. Jetzt:
  die Basis wandert mit der Formation auf und ab (`baseShift = (macro - 0,5) * 0,20`, also +-0,10 Dicken), der Anstieg ist weicher (0,10), der Carve ist am Boden stärker (+0,22, zerfranste Unterseite),
  und der Marsch beginnt 0,12 Dicken unter der Nennbasis, damit Dichte unterhalb entstehen kann. `cloudFieldDensity` (Sonnenmarsch, Wolkenschatten) nutzt dieselben Funktionen und folgt.
* **Oberflächen-Detail einmal pro Pixel** (`tAcc/wAcc` = deckungsgewichtete mittlere Entfernung der sichtbaren Oberfläche): dort wird das Licht mit einem kleinen Worley-Feld moduliert
  (Hang zur Sonne heller, Mulde dunkler, 3 Fetches pro Pixel, nur nah: 2 bis 12 % von `maxDist`). Das Detail liegt bewusst nicht in der Dichte: die Marschschritte sind gröber als jede sinnvolle
  Oktave (siehe `docs/clouds-2-0-cost-and-fine-detail-2026-10-29.md`), im Licht ist es von Schrittweite und Sprung-Offset unabhängig. Lump-Maßstab 6,5 (Zellen ~100 Einheiten); eine erste, feinere Fassung (11) sah zerknittert aus.
* **Dünne Fahnen sparen den Sonnenmarsch**: unter Deckkraft 0,04 (`qSkipA`) nimmt ein Schritt ein typisches Licht (0,8). Bild +0,2 von 255, spart ~1,7 ms (der weichere Boden erzeugt viele solcher Fahnen). High (`qSkipA` 0) überspringt nie.
  Ein Test mit 0,10 sparte weitere 0,7 ms bei 1,1 von 255 Bildänderung: nicht übernommen.

## Gemessen (Metal, M5, 2840x1528, `scripts/perf/sky_cloud_cost.py --repeat 5`, Scene-Pass p50 in ms)
| Variante | Stand 8dfb8641 | nach Unterseite + Detail + Skip |
|---|---|---|
| Low | 15,7 | 15,9 bis 16,6 |
| **Medium** | **20,9** | **22,5 bis 23,4** (+8 bis +12 %) |
| High | 33,1 | 37,8 (nur ein Lauf, schlechtere Bedingungen) |
| Medium + Low-Res | 7,9 | 8,0 bis 8,7 |
Aufschlüsselung (jeweils min von 5): weichere Unterseite allein Medium +3,8 ms (24,7), das Detail ~0 (24,6), die sanftere Unterseite (+-0,10, 0,10, 0,12) spart nur 0,4 (24,2), der Skip 1,7 (22,5).
Die beiden Läufe am Ende liegen ~5 % auseinander (Fremdlast: andere Apps auf der GPU); innerhalb einer Serie vergleichen.

## Verifiziert
Metal + OpenGL, `HE_SKY_TIME` fest: die zehn Bilder ohne Realistic-Wolken bitgleich zur Baseline; Realistic weicht vom vorigen Commit im Mittel 2,7 bis 5,2 von 255 ab
(mehr Wolkenvolumen unten, helleres Overcast: mittlere Helligkeit 143 -> 149, Medium 183 -> 186). Metal gegen OpenGL 0,6 bis 1,1 von 255 (Low 2,5, wie vorher). `ctest` voll 345/345 (privates HOME);
Test `test_culling` erweitert (Sonnenmarsch-Skip in GL und MSL gleich, High 0).

## Nicht geprüft / offen
* **Die Unterseite ist verbessert, nicht perfekt**: bei 100 m unter der Basis bleibt sie weich und fast ohne Struktur. Wer echte Fransen will, braucht mehr Detail in der Dichte (feinere Schritte).
* High nur mit einem Lauf gemessen (37,8 gegen 33,1 vorher): die Zunahme ist größer als bei Medium, nicht untersucht.
* Überprüft nicht: D3D11/D3D12/Vulkan zur Laufzeit; Dämmerung (sah wie vorher aus), Classic und Dome nicht berührt.
* Medium ist jetzt 22,5 ms gegen 20,9 ms vor diesem Schritt: das ist der Preis für weichere Unterseiten (mehr Wolkenmasse). Wer das Budget braucht: Low-Res (8,0 ms) oder die Basisverschiebung auf 0,15 und die Marschverlängerung auf 0,06 zurücknehmen.

## Handoff
* Nächste Schritte laut `docs/clouds-2-0-survey-2026-10-29.md`: Wolkentypen (4), Horizont (6), Dome (5); bei den Kosten weiter am Sonnenmarsch.
* Die Konstanten (`baseShift` 0,20, Anstieg 0,10, Verlängerung 0,12, Lump-Maßstab 6,5, Gewichte 0,35/0,50, `qSkipA` 0,04) sind Augenmaß; geändert wurde immer in GL und MSL zugleich.
