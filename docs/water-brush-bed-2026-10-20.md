# Wasser-Pinsel: Bett aus Ufer-Abstand, Max Depth / Slope / Bed Roughness (Thema 206, Schritt 9)

Stand 20.10.2026, macOS, Release. Auslöser: der Nutzer meldete (1) Wasser-Material wirkt beim Pinsel nicht richtig,
(2) das Landscape wird beim Malen nicht richtig verformt, und wollte (3) Max Depth und Slope getrennt, (4) ab einer
Distanz zum Ufer die volle Tiefe, auch beim weiteren Malen, (5) eine neue Einstellung Bed Roughness.

## Ursachen (aus dem Code und den Zahlen, nicht aus einem gerenderten Bild abgelesen)

1. **Material.** Seit Thema 206 Schritt 2 backt `WaterMesh.cpp` die Wassertiefe ins Mesh (`e = 1 − Tiefe / kShoreFadeDepth`,
   `kShoreFadeDepth` = 1 m, `WaterMesh.cpp:163-173`), und `Water.hasset` macht daraus Alpha-Rampe, Schaumsaum und
   durchscheinenden Grund. Ein Pinsel-Teich ohne Bett (Dig Bed war aus, Spiegel = Gelände + 0,3 m) ist überall höchstens
   0,3 m tief, liegt also **komplett im Uferstreifen**: nur Schaum und Klarwasser, kein Körper, kein Tiefenverlauf. Das Material
   ist richtig angewendet, es bekommt nur nie Tiefe zu sehen. Beheben lässt sich das nur über ein Bett.
2. **Verformung.** `dab()` grub mit `TerrainSculpt::lowerToFloor` nach dem Gewicht des Pinsels (voll im Innenkreis, Spiegel bei
   Gewicht 0,5), danach räumten `keepUnderSheet` und `shapeBank` auf. Das Ufer ging nie in das Ziel ein: der Boden wurde eine
   Mulde in Pinselform mit Wänden im Falloff, bei einem Zug eine Rinne, die dem Cursor folgt, und nach weiterem Malen
   wanderte die Tiefe nicht mit dem Ufer mit.

## Was jetzt gilt

`WaterBrush.cpp` `digBed()`: Jeder Eckpunkt unter dem Wasser geht auf `Spiegel − min(maxDepth, slope · d) · (1 + roughness · n)`,
`d` = Abstand zur Kontur der nassen Zellen (`shoreSegments` des ungeclippten Gitters, nicht zur geclippten Kante: die wandert
mit dem Absenken und würde jedes Ziel mitziehen), `n` = Rauschen in −1..1 aus der Position allein (zwei Oktaven Value-Noise,
~7 m und ~2,5 m, kein Zufallszustand). Volle Tiefe also ab `maxDepth / slope` Metern vom Ufer; der Panel zeigt den Wert.
Wer das Ufer mit weiterem Malen nach außen schiebt, vergrößert `d` dahinter, und der nächste Dab gräbt das Gelände bis
`kMaxBedReach` (64 m) um den Dab herum entsprechend tiefer. Unverändert: nur unter dem Wasser (`keepUnderSheet`, Thema 188),
hebt nie, Gelände über dem Spiegel unter dem Wasser geht sofort auf den Spiegel, `shapeBank` formt die trockene Böschung.
Der Distanz-Sweep von `shapeBank` ist als `shoreField()` herausgezogen und wird von beiden benutzt.

Einstellungen (`HE::water::BrushBedParams`, `WaterField.h`; `Params::digDepth` = Max Depth, neu `bedSlope`, `bedRoughness`):
Max Depth 0..200 m, Slope 0,01..2 m/m (Standard 0,25), Roughness 0..1 (Anteil der Tiefe). Sie liegen am Körper (`Body::brushBed`),
werden von jedem Dab gesetzt (`setBrushBed`), stehen in `sameContent`/`Delta`, in der Szene unter `brushBed` **nur wenn vom Standard
abweichend** (also nur für gegrabene Körper; ältere Szenen laden mit Standard, ein ungegrabener Körper speichert byte-gleich wie
vorher, beides im Test) und werden in `sanitize` geklemmt. Keine Versionserhöhung nötig (JSON-Schlüssel, kein Chunk).

Editor (`TerrainTools.cpp`): Water-Tab, Abschnitt Ground: Dig Bed, **Max Depth**, **Slope**, **Bed Roughness**, darunter „Full depth N m
from the shore“. **Dig Bed ist jetzt standardmäßig an** (Max Depth 2 m, Slope 0,25, Roughness 0,2), siehe Ursache 1. Hilfetexte
`EditorHelp.cpp` (`Landscape/Dig Bed|Max Depth|Slope|Bed Roughness##water`, der alte `Depth`-Eintrag ist ersetzt).

## Verifiziert

- `he_tests -tc="Water brush*,landscape ui*"` 25 Fälle grün; neu: `Water brush bed: …` (4 Fälle: Profil Slope/Max Depth gegen die
  gemessene Kontur, Ufer nach außen schieben vertieft den Kern bis Max Depth, Roughness deterministisch und im Anteil, Speichern/Laden
  inkl. Szene ohne Schlüssel). Der Thema-188-Invariantentest („keine gegrabene Ecke bleibt trocken“) läuft unverändert grün.
- `editor_help_audit.py --check` ok (1204/1204). `HorizonEditor` baut und linkt (Release, Assimp aus); die Panel-Änderung ist durch
  `landscape ui` im Test geklickt, nicht im laufenden Editor gesehen.
- Voller `he_tests`-Lauf: 5611 von 5615 grün. Die 4 Roten sind nicht aus dieser Änderung: `Disk registry: scanContentDirectory…`
  (`test_hpak.cpp:1188`, schon vorher rot, siehe Handoff Thema 174) und zwei Profiler-Fälle (`test_profiler.cpp:107/215`, liefen
  neben dem Editor-Link, zeitabhängig). Der vierte, `insideSheet`, kam von meinem Splitter-Filter in `sheetCovers` und ist behoben;
  danach `Water*,landscape ui*,*sync*,Scene*` 435/435.
- Angepasste ältere Tests: der Dig-Fall (`Water brush dig: the ground under the water…`), der Konvergenz-Fall (Slope 2), der UI-Fall
  (Dig Bed erst aus, dann an, wegen des neuen Standards), das Halten-Dab-Fenster (24 Dabs bis die Deckung sättigt statt 10).
  `sheetCovers` in `test_water_brush.cpp` überspringt jetzt Splitter-Dreiecke: ein flaches Dreieck „deckte“ jeden Punkt auf der
  Verlängerung seiner Kante, das hat bei der neuen Geometrie eine 4-m-Wand vorgetäuscht, die es nicht gab.

## Nicht verifiziert

- Kein gerendertes Bild: weder `HE_DUMP_WATERBRUSH` noch ein Blick im Editor. Dass der Teich jetzt „nach Wasser aussieht“, folgt aus
  Tiefe → `e` → Material, ist aber nicht gesehen. Die Witnesses in `EditorApplication.cpp` (7964/7968/8240/8305) setzen weiter nur
  `digDepth` und bekommen jetzt Slope 0,25: ihre Bilder und Zellzahlen ändern sich im Boden.
- D3D/Vulkan (nicht auf dem Mac prüfbar), Mehrspieler-`TerrainSync` (`brushBed` steckt nicht im Hash von `TerrainSync.cpp:90`, der nur
  Seen mit `digsBed` hasht; es reist über die Szenenserialisierung/`sameContent`).

## Entscheidungen und Vorschläge

- Entschieden: eine Tiefe (`maxDepth`) und eine Steigung statt einer dritten „Distanz“-Einstellung (die Distanz folgt aus beiden und wird
  angezeigt); Rauschen als Anteil der Tiefe, damit die Uferlinie sauber bleibt; Dig Bed an.
- Vorschlag: Seen (Spline-Werkzeug, `dugDepth/dugBank`) auf dasselbe Ufer-Profil umstellen, das ist hier nicht angefasst. Ein Eraser lässt
  das Bett wie es ist (hebt nie).
- Falle: Der Build-Ordner wurde mit `-DHE_ENABLE_ASSIMP=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON` neu konfiguriert (Assimp bricht mit
  der CLT-SDK-Include-Mischung ab). Zurück: `cmake -S . -B cmake-build-release -DHE_ENABLE_ASSIMP=ON -DFETCHCONTENT_FULLY_DISCONNECTED=OFF`.

## Nächste Schritte

1. Editor starten, Wasser-Tab: Teich mit Standardwerten malen, Slope/Max Depth/Roughness ziehen, `HE_DUMP_WATERBRUSH=dig` auf Metal/GL
   neu schießen und in `docs/water-brush/` ablegen.
2. Entscheiden, ob der Spline-See dasselbe Profil bekommt.
3. Windows/Linux-CI abwarten.
