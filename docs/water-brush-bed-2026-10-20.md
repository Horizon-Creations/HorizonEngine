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

## Nachtrag: Close Bed on Erase

Auftrag: Beim Erase soll das vom Pinsel gegrabene Bett wieder geschlossen werden, auf Wunsch per Checkbox.

- **Datenmodell:** Jeder Dab mit Dig summiert, was er am Gelände verändert hat (Höhe danach minus davor, Senken negativ), in
  `Body::brushPatch` (`GroundPatch`, wie beim See). Aufgenommen wird der ganze Bereich, den ein Dab bewegen kann, die Böschung
  eingeschlossen (`recordPatch`, `WaterBrush.cpp`). Gespeichert im Schlüssel `brushBed` (`res`, `rect`, `deltaB64`), in `sameContent`
  verglichen, in `sanitize` verworfen, wenn er nicht zum Höhenraster passt. Ältere Szenen haben ihn nicht und laden wie vorher.
- **Verhalten:** `Params::closeBed` (Standard im API aus). Beim Erase gibt `closeBedAt` jedem Eckpunkt, dessen Wasser weg ist (`bodyAt`
  ist nicht mehr der Körper), `Stärke · Pinselprofil` seines Anteils zurück und verkleinert den Patch um den zurückgegebenen Teil.
  Zurück geht nur der Anteil des Pinsels, später selbst Gesculptetes bleibt; unter Wasser, das stehen bleibt, bleibt das Bett.
  `removeCircle` löscht einen Körper ohne Zellen samt Patch: deshalb nimmt der Dab die Patches vorher mit, und ein verschwundener
  Körper wird in diesem Kreis mit voller Stärke geschlossen (danach kommt nichts mehr zurück). Der Rest außerhalb des Kreises bleibt
  dann offen, wenn die Maus vorher losgelassen wird.
- **Editor:** Checkbox „Close Bed on Erase“ im Abschnitt Ground des Wasser-Tabs, Hilfeeintrag `Landscape/Close Bed on Erase##water`.
  **Standard an.** Grund: Wer Wasser wegradiert, will in aller Regel den Teich weg haben und kein trockenes Loch behalten; die Option
  fasst nur Gelände an, das der Pinsel selbst gegraben hat (der Patch), also nie fremdes Sculpting. Aus = Verhalten wie bisher.
- **Tests (`test_water_brush.cpp`):** Bett öffnen, mit Erase wieder auf die alte Höhe schließen (Abweichung < 5 cm, nie über dem
  Original), mit der Checkbox aus bleibt es offen (bit-gleich dem offenen Zustand), weit entferntes Erase ändert nichts, Patch
  übersteht Speichern/Laden und schließt danach noch. `Water*,landscape ui*` 229/229, `editor_help_audit.py --check` ok,
  `HorizonEditor` baut.
- **Nicht verifiziert:** nicht im laufenden Editor gesehen; ein Erase mit niedriger Stärke, bei dem der Körper schon vor dem Ende
  des Strichs verschwindet, wurde nur über die Logik bedacht, nicht eigens getestet. Der Undo-Schritt nimmt Gelände mit (Snapshot).

## Nachtrag: Wasser-Material bleibt gebunden (Schritt 9c)

Meldung: Das Material werde beim Malen und nach dem Wiederöffnen der Szene nicht richtig gebunden.

- **Befund:** `WaterSurface.cpp` `Pass::apply` rief `ensureWaterMaterial(cm)` und setzte das `MaterialComponent` nur beim Anlegen
  einer Oberfläche (`existing == entt::null`). Eine Oberfläche, die weiterlebt, bekam weder eine Residenz-Prüfung noch eine Reparatur:
  wird das Material danach entladen (Projektwechsel, Unload-Sweep, Szenenwechsel) oder fehlt/zeigt das Component auf anderes (Snapshot,
  Skript, ältere Szene), zeichnet der Renderer mit dem grauen Fallback, bis die Oberfläche neu gebaut wird. Beim ersten Malen passt der
  Anlegepfad; das Problem liegt bei allem, was danach kommt. Die Log-Warnung kommt nicht, weil die Datei gefunden wird.
- **Fix:** `Pass::terrain` prüft jetzt in jedem Tick, noch vor dem Steady-Return, alle vorhandenen Oberflächen: Material nicht resident →
  `ensureWaterMaterial` (bei einem Fehlschlag höchstens einmal pro 60 Ticks, Zähler `Field::materialWait`, Laufzeit, nicht gespeichert);
  `MaterialComponent` fehlt oder zeigt auf etwas anderes → auf `kEngineWaterMaterialId` gesetzt, `dirty`.
- **Test:** `test_water_surface.cpp`, „a surface that already exists is given its material back“ (Material entladen → wieder geladen;
  Component entfernt + entladen → beides zurück; Component auf Null-UUID → Wasser-Material). Mit `Water*,landscape ui*,*terrain*,
  *engine material*,*Scene*` 509/509 grün, `HorizonEditor` baut.
- **Nicht verifiziert:** Ein Negativ-Lauf ohne den Fix wurde nicht gemacht (der Test wurde mit dem Fix geschrieben), und im echten Editor
  habe ich weder das Wiederöffnen noch das Malen gesehen; ich konnte den Fehler des Nutzers nicht an seinem Deploy reproduzieren. Wenn
  es danach noch grau bleibt, ist die Ursache woanders (Renderer löst die Material-UUID nicht auf / Shader-Kompilierung): dann in
  `HorizonEngine.log` nach Material-Fehlern suchen und `HE_DUMP_WATERLAKE` auf Metal schießen.
