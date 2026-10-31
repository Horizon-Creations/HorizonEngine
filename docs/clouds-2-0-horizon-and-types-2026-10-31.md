# Wolken 2.0, Schritt 4f: Horizont und Wolkentypen (Thema 227, Hive-Schritt 1140, sechster und letzter Teil)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `36a4c7f1`. Gilt für die Realistic-3D-Wolken (`applyClouds3DReal`, `cloudColumnReal`, `cloudBodyReal`), GL-Quelle `SkyShaderSource.h` und Metal-Spiegel.
Bild (Metal, oben zwei Reihen: Typ 0..4 bei 15 und 5 Grad; unten: Horizont vorher/nachher bei 2 Grad und bei Bedeckung 0,9): `docs/clouds-2-0-survey/horizon-and-types-2026-10-31.jpg`.

## Was geändert ist
* **Horizont / Luftperspektive (Maßnahme 6).** Der Dunst war eine späte lineare Rampe (`smoothstep(0,35 * maxDist, maxDist, t)`, höchstens 60 %); jetzt Beer-Dämpfung entlang des Strahls
  (`1 - exp(-t / (0,20 * maxDist))`, bis 90 % zur Himmelsfarbe der eigenen Richtung). Der Streifwinkel-Ausblendebereich ist von `elevFloor+0,14` auf `+0,09` verengt, weil der Dunst die
  ferne Platte jetzt selbst schluckt: die Wolken laufen weiter nach unten in den Dunst aus statt an einer Linie zu enden. Nur Realistic; Classic, Dome, Overcast-Deck unberührt.
* **Wolkentypen (Maßnahme 4).** Neues Feld `cloudType` (0 Gemischt = bisheriges Bild, 1 Flache Cumulus, 2 Hohe Cumulus, 3 Stratocumulus, 4 Cumulonimbus). Ein Typ setzt
  Zellgröße (Faktor auf die Frequenz der Wetterkarte) und Höhenprofil (unterste Turmhöhe, Turmhöhen-Faktor, Obergrenze); `coverage` bleibt die Menge. Tabelle `cloudShapeParams` in GL und MSL.
  Typ 0 multipliziert überall mit 1 und deckelt nichts.
* Kette wie bei `cloudDomeHeight`: `EnvironmentSettings`/`EnvironmentComponent`/`EnvironmentPush`, `HE_ENV_FIELDS_INT` (env.get/setCloudType, damit Lua/Python/HorizonCode und die Szene),
  Inspector-Kombi "Cloud Type" (nur Realistic), Hilfe, `HcNodeDocs`, `HE_DUMP_CLOUDTYPE`.
* **Transport:** neues `SkyFrameParams::shape` (x = Typ), 352 -> 368 Byte; Vulkan/D3D-Prelude (`#define uCloudType`), GL-Uniform `uCloudType` (beide Push-Stellen), MSL `SkyParams.shape`.
  Metal reicht den Typ in `drift.w` an die Realistic-Funktionen (dort ungenutzt; Dome-Funktionen bekommen weiter die Dome-Höhe): `float4(p.drift.xyz, p.shape.x)` an drei Einstiegsstellen.
* `scripts/perf/sky_cloud_cost.py`: Varianten `type1..type4`.

## Geprüft (Mac, Release, Metal + OpenGL, `HE_SKY_TIME` 300, AA aus, Bild 1280x720)
* Sieben Zeugen ohne Realistic-Wolken (Dome 3x, Classic 3x, klar) **bitgleich** vor und nach beiden Änderungen. Typ 0: alle acht Realistic-Bilder **bitgleich** zum Stand nur mit Horizont.
* Horizont gegen `36a4c7f1`: Realistic im Mittel 1,5 bis 4,7 von 255 anders (Max 30..38). Typen 1..4 sind klar verschieden (Zellgröße, Höhe). Metal gegen GL im Mittel 0,6 bis 1,3 von 255 (wie zuvor).
* Kosten (`sky_cloud_cost.py`, Scene-Pass p50, M5, 2840x1528, Coverage 0,55): Medium 21,7 (vorher dokumentiert 22,5 bis 23,4), High 34,7 bis 35,9 (33 bis 37,8), also kein Mehraufwand durch Horizont und Typ 0.
  Typen bei Medium (min von 3): flach 20,1, hoch 25,6, Strato 18,8, Cumulonimbus 32,8 (gemischt 22,4): große, hohe Zellen kosten mehr Wolkenmasse.
* Tests: `test_culling` (Struktur, Typ-Tabelle gleich in GL/MSL, Typ klemmt 0..4), `test_sky_shader` (Feldordnung, `heSky_shape : packoffset(c22)`), `test_weather`, `test_scene_serializer`, `test_engine_api`;
  `editor_help_audit.py --check` grün. Vollständiger ctest: siehe Hive-Post.
* Gegen die Referenzbilder des Themas (Dateien 69 bis 76): Typ 1 liest sich wie Referenz 4 (kleine flache Cumulus, die zum Horizont schrumpfen), der Horizont läuft in Dunst aus.

## Nicht geprüft / offen
* **D3D11, D3D12, Vulkan zur Laufzeit** (Schritt 1141): Gerät NN-WS03 war offline. Die Shader gehen durch dieselbe GLSL-Quelle; nur die Struktur-Tests und die HLSL-Offsets sind geprüft.
* **Oberflächen-Detail bleibt der große Abstand zu den Referenzen**: Referenzen zeigen scharfe Blumenkohl-Wülste, hier sind Ränder weich; Cumulonimbus liest sich als glatte Masse, nicht als Turm.
  Ursache und Grenze stehen in `docs/clouds-2-0-cost-and-fine-detail-2026-10-29.md` (feinere Oktaven brauchen feinere Marschschritte). Zusätzlich leichte horizontale Schichtung bei Typ 2 und 4 aus der Schrittweite.
* Die Typ-Konstanten (Zellfaktor 1,55/0,85/1,25/0,50; Turmprofile) und Dunst (0,20 / 0,9, Fenster 0,09) sind Augenmaß, in GL und MSL gleich geändert. Dämmerung nicht eigens untersucht.
* Maßnahme 5 (Realistic-Feld im Dome-Modus) nicht gemacht: eigener Posten, Aufwand L.

## Handoff
* Schritt 1140 fertig. Nächster Schritt 1141: Backend-Abnahme auf Windows (NN-WS03), `he_shot`-Reihe mit `CLOUDTYPE=0..4` auf D3D11/D3D12/Vulkan, Bild gegen Metal; die Lavapipe-CI auf diesem Zweig liefert ein Vulkan-Bild.
* Wer als Nächstes am Aussehen arbeitet: zuerst das Oberflächen-Detail (Ausgabe im Licht statt in der Dichte, siehe Abschnitt "Nicht geprüft"), dann Maßnahme 5.
