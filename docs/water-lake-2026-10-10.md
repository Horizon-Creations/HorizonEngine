# Lake-Werkzeug und Interoperabilität mit dem Pinsel (Thema 174, Schritt 8)

Stand 10.10.2026, macOS (Metal und OpenGL), Debug-Build. Schritt 3 lieferte das gemeinsame Wassermodell
(`HE::water::Field`), Schritt 4/5 die Fläche samt Ufer-Beschneidung, Schritt 6 den Aushub unter einem Polygon,
Schritt 7 den Wasser-Pinsel. Dieser Schritt ist das zweite Werkzeug, das in dasselbe Modell schreibt: eine
**geschlossene Spline wird ein See**. Punkte verschieben formt das Wasser um und nimmt seit Thema 188 Schritt 2 das
gegrabene Bett mit (Nachtrag am Ende), und was der Pinsel an einem See hinzugefügt oder weggenommen hat, bleibt beim Umformen
erhalten.

Code: `src/HE_Scene/include/HorizonScene/WaterLake.h` + `src/HE_Scene/src/WaterLake.cpp` (Kern, ohne ImGui;
`HE::water::lake`), `Body::polygon` in `WaterField.h` (+ Speichern in `SceneSerializer.cpp`, Schlüssel
`polygonB64`), der Sync in `TerrainSystem::updateTerrains` (vor `WaterSurface::update`), Editor in
`src/HE_Editor/SplineTool.cpp` (Abschnitt „Lake“ im Spline-Panel) und `TerrainTools.cpp` (Liste „Bodies“ mit
„To Lake“ im Wasser-Modus), Hilfetexte in `EditorHelp.cpp`, Engine-API `HE::api::water` in `EngineApi.cpp`
(14 Registry-Zeilen), MCP-Werkzeug `terrain_lake` in `McpToolsTerrain.cpp`, Zeuge `HE_DUMP_LAKEEDIT` in
`EditorApplication.cpp`. Tests: `tests/test_water_lake.cpp` (33 Fälle), `tests/test_water_lake_api.cpp` (7),
`tests/test_mcp_tools_terrain.cpp` (+6), `tests/test_spline_tool_ui.cpp` (+2) und `tests/test_terrain_tools_ui.cpp` (+1).

## So benutzt man es

1. **Zeichnen.** Im Spline-Modus eine Linie auf das Landscape klicken und mit „Closed“ schließen (mindestens
   drei Punkte). Die Punkte lassen sich jederzeit verschieben, einfügen und löschen, wie in Schritt 2.
2. **Lake-Abschnitt im Panel** (erscheint bei einer geschlossenen Spline über einem Landscape): *From Ground* /
   *Above Ground* oder *Level* (Wasserspiegel), *Dig Bed*, *Depth* (Tiefe), *Bank* (Randverlauf), dazu *Clip To
   Ground* und *Shore Overshoot* (Beschneidung). **Create Lake** gräbt das Bett und legt das Wasser darüber,
   in **einem** Undo-Schritt.
3. **Umformen.** Punkte ziehen, einfügen oder löschen, auch den ganzen Spline-Entity bewegen oder das Landscape
   verschieben: das Wasser folgt noch im selben Frame (`syncSplines`), und ein See, der sein Bett selbst gegraben hat,
   nimmt es mit (seit Thema 188 Schritt 2, siehe Nachtrag). **Dig Again** gräbt das Bett unter der *aktuellen*
   Kontur neu, ein Undo-Schritt (und keiner, wenn nichts
   tiefer wurde). Der Wasserspiegel ist ein eigener Regler (ein Undo-Schritt je Zug).
4. **Mit dem Pinsel erweitern.** Ein Strich, der auf dem See beginnt, setzt diesen See fort (Schritt 7). Was er
   außerhalb der Kontur malt oder mit dem Radierer aus ihr herausschneidet, ist der *Pinsel-Anteil* und bleibt
   beim Umformen (Regel unten).
5. **Pinselwasser in einen See umwandeln.** Im Wasser-Modus des Landscape-Panels listet „Bodies“ jeden Körper:
   Teiche (Pinselwasser) haben **To Lake**, Seen **Select Spline**, Seen ohne Spline **Detach**. To Lake liest die
   Kontur des größten Wasserstücks, legt eine geschlossene Spline (höchstens 32 Punkte) als Kind des Landscape
   darum, schaltet auf den Spline-Modus und wählt sie aus. Ein Undo-Schritt.

![Ein gezeichneter See (Metal): Spline-Punkte, Bett mit Böschung, Wasserfläche](water-lake/metal-drawn.png)

## Daten: eine Kontur am Körper

`Body` bekam `polygon`: die Kontur (terrain-lokales XZ, ohne schließenden Doppelpunkt), aus der die Zellen
**zuletzt** gelegt wurden. Pinselwasser hat keine. Sie ist die Vergleichsbasis der Regel unten, und sie
ist bewusst *keine* zweite Rasterebene: eine Kontur ist wenige hundert Punkte klein, hängt nicht an der Auflösung
und wird mit der Szene gespeichert (`polygonB64`, rohe Float-Paare wie `sculptHeights`; `sanitize` verwirft
Nicht-Zahlen, Überlänge und eine Kontur an Pinselwasser). `syncKey` am Körper ist reine Laufzeit (nie gespeichert,
nie verglichen): der Fingerabdruck von Spline-Punkten, `closed` und beiden Weltmatrizen, mit dem der Sync merkt, dass
sich etwas bewegt hat. Neu geladen oder per Undo zurückgeholt ist er 0: der erste Blick **merkt sich nur**, er formt
nichts um.

## Die Regel: Pinsel-Anteil über Spline-Anteil

Zwei Anteile Wasser hat ein See. Der **Spline-Anteil** ist, was seine Kontur sagt. Der **Pinsel-Anteil** ist der
Unterschied zwischen den Zellen, wie sie jetzt sind, und dem, was die Kontur von damals (`Body::polygon`, „alt“) ergibt.
Er wird nicht mitgeführt, sondern beim Umformen aus den Zellen abgelesen. Mit `altR`/`neuR` als Kontur auf das Raster
des Feldes gebracht (anti-aliased, Deckung 0..255):

| Pinsel-Anteil | Wie er erkannt wird |
|---|---|
| **hinzugefügt** | Zelle, die der Körper besitzt, mit *mehr* Deckung als `altR` sagt |
| **weggenommen** | Zelle mit *weniger* Deckung als `altR` sagt, die der Körper noch besitzt oder die trocken ist (eine Zelle, die ein anderer Körper genommen hat, ist dessen, kein Radieren) |

**Neue Zellen = ( `neuR` ∪ hinzugefügt ) − weggenommen**, wobei ∪ die größere Deckung nimmt und − die Zelle auf das
herunterbringt, was der Pinsel dort stehen ließ (0 beim vollen Radieren, ein Zwischenwert beim weichen). Danach ist
die neue Kontur die Basis. Die sechs Folgen, jede ein Testfall in `tests/test_water_lake.cpp`:

1. Verkleinert man die Kontur, **trocknet** der Bereich, den nur die alte Kontur deckte.
2. Vergrößert man sie, **nimmt sie Pinselwasser auf**, das sie jetzt deckt; nichts verdoppelt sich.
3. Pinselwasser **außerhalb beider Konturen bleibt**, wo es gemalt wurde (eine Rinne aus dem Ufer heraus).
4. Eine **Kerbe**, die der Radierer innen schnitt, **bleibt eine Kerbe**, auch wenn die Kontur wächst oder wandert,
   solange sie innerhalb liegt.
5. Eine Zelle, die die neue Kontur ordentlich deckt (Deckung ≥ `kWet`), wird **einem anderen Körper abgenommen**,
   wie bei `addPolygon`: das Polygon ist maßgeblich.
6. Dieselbe Kontur noch einmal ändert **nichts** (`changed == 0`, Feld bit-gleich).

Was die Regel nicht kann (und dokumentiert statt gelöst ist): Eine Kerbe, die beim Umformen **außerhalb** der neuen
Kontur landet, ist danach trocken *und* außerhalb, es gibt nichts mehr zu vergleichen; wandert die Kontur zurück, ist
die Kerbe verheilt (Test 4b). Und `setResolution` tastet die Zellen neu ab, nicht die Basis; das nächste Umformen
danach kann einen dünnen Ring „Pinsel“ entlang des alten Ufers lesen. Beides verliert kein Wasser.

Pinsel über Wasser, das die Kontur schon deckt, fügt nichts hinzu (die Zelle ist schon nass): ein Strich, der
innen beginnt und innen endet, ist für die Regel unsichtbar. Das ist gewollt, es gibt nichts zu erhalten.

![Der See mit Rinne (Pinsel, gegraben) und Insel (Radierer)](water-lake/metal-extended.png)

![Dieselbe Szene, Punkte verschoben: Kontur gewandert, Rinne und Insel bleiben](water-lake/metal-reshaped.png)

Das zweite Bild zeigt den Stand von Thema 174, „nur das Wasser“ (seit Thema 188 Schritt 2 nur noch für einen See ohne
eigenes Bett): Wo die Kontur zurückgezogen wurde, bleibt die alte Grube
als trockene Mulde (dunkler Rand); wo sie vorgezogen wurde, liegt das Wasser flach auf ungegrabenem Boden, ohne Tiefe.
**Dig Again** gräbt beides nach, wenn man es will.

## Was welche Aktion anfasst

| Aktion | Wasser | Boden | Undo |
|---|---|---|---|
| Create Lake | ja | ja (`excavatePolygon`, Floor-Modus, nur Absenken) | 1 Schritt |
| Punkt ziehen / einfügen / löschen, Spline oder Landscape bewegen | ja, automatisch (`syncSplines`) | ja, wenn der See sein Bett gegraben hat (`Body::digsBed`): altes Bett angehoben, neues gegraben, jeden Frame des Ziehens | der Schritt der Spline-Bearbeitung |
| Dig Again | nein | ja, unter der aktuellen Kontur | 1 Schritt, keiner ohne Änderung |
| Wasserspiegel | ja (`setLevel`) | nein | 1 Schritt je Zug |
| Remove Lake | ja | nein | 1 Schritt |
| To Lake | ja (Kontur ersetzt das Stück) | nein | 1 Schritt (Spline-Entity inklusive) |
| Beschneidung (Clip To Ground, Shore Overshoot) | Fläche, nicht Zellen | nein | 1 Schritt je Änderung |

Die **Beschneidung** ist eine Landscape-Einstellung (`Field::clipToGround`, Schritt 5), keine je See: sie gilt für
Seen und Pinselwasser gleichermaßen, weil sie im Netzbau sitzt, nicht in den Zellen. Das Lake-Panel zeigt sie, damit
man sieht, wie der See ans Ufer stößt; sie dort zu ändern ändert sie für das ganze Landscape.

**Wasserspiegel „aus dem Boden“** nimmt den tiefsten Boden unter der Kontur (vor dem Graben gelesen) plus *Above
Ground*. Das Bett liegt `Depth` darunter, die Böschung `Bank` Meter weit **nach innen** von der Kontur (Smoothstep,
seit Schritt 13 — vorher nach außen, siehe unten).

## To Lake: Kontur extrahieren

`extractOutline` liest die Konturen des Körpers aus demselben Gitter, aus dem die Fläche gebaut wird
(Marching Squares), nimmt die äußere mit der größten Fläche, vereinfacht sie per Douglas-Peucker (Toleranz anfangs
eine Zelle, wächst, bis höchstens 32 Punkte übrig sind; eine Catmull-Rom-Spline durch 300 Punkte wäre unbenutzbar)
und zählt, was sie auslässt (`pieces`, `islands`). `adopt` macht den Körper zum See dieser Spline: das Hauptstück
(Kontur, eine Zelle weich erweitert) wird durch die Füllung der Spline-Kontur ersetzt, Spiegel und Id bleiben.
Bewusst:

- Der **ausgefranste Pinselrand wird zur glatten Spline**; kein Geisterrand, der beim Umformen als „Pinsel“ stehen bliebe.
- **Inseln werden gefüllt**; mit dem Radierer wieder herausschneiden, das bleibt dann bei jedem Umformen.
- Ein **getrenntes kleines Stück** desselben Körpers (weiter als eine Zelle weg) bleibt als Pinselwasser.

![Ein gemalter Teich nach „To Lake“: 20 Spline-Punkte, 2287 → 2275 Zellen](water-lake/metal-converted.png)

## Engine-API, HorizonCode, Lua/Python, MCP

Gruppe `water` in `HE::api` (Registry, `isScriptGroup("water")`, Anzeigenamen, `HcNodeDocs`, Tests), Koordinaten und
Spiegel in **Weltraum** wie die `nav`-Gruppe:

| Zeile | Art | Was |
|---|---|---|
| `water.createLake(terrain, spline, level, dig, depth, bank)` → body | exec | See aus geschlossener Spline, Spiegel als Welt-Y |
| `water.createLakeAtGround(terrain, spline, above, dig, depth, bank)` → body | exec | Spiegel = tiefster Boden unter der Kontur + `above` |
| `water.reshapeLake(spline)` → changed | exec | jetzt umformen (der Tick tut es ohnehin), −1 = kein See |
| `water.digLake(spline, depth, bank)` → lowered | exec | Bett neu graben |
| `water.setLakeLevel(spline, level)` / `water.removeLake(spline)` | exec | Spiegel (Welt-Y) / See entfernen |
| `water.convertToLake(terrain, body)` → spline | exec | Teich → See, legt die Spline an |
| `water.paint` / `water.erase (terrain, x, z, radius, falloff)` → cells | exec | ein Pinselklick (Welt-XZ) |
| `water.bodyAt`, `water.levelAt` (wet, level), `water.lakeBody`, `water.lakeSpline`, `water.wetCells` | pure | Fragen |

MCP: `McpToolsApi` macht nur **reine** Zeilen zu Werkzeugen (Exec-Zeilen schreiben ohne Undo), also sind die fünf
Fragen sofort als `api_water_*` da. Für das Anlegen und Umformen gibt es **`terrain_lake`** (`McpToolsTerrain.cpp`,
nach dem Muster von `terrain_sculpt`): Aktion `create | reshape | dig | level | remove`, die Spline als Entity-Uuid, alle
Höhen in Welt-Y; es arbeitet auf einer **Kopie** des Landscape, schreibt das Ergebnis über das Command-Gateway zurück
(ein Undo-Schritt für Wasser und Boden zusammen), meldet „nichts geändert“ ohne Undo-Eintrag und lehnt mit den Codes
ab, nach denen ein Client verzweigt (`invalid_payload`, `not_found`, `play_mode`). `body` bei `create` macht einen
vorhandenen Teich zum See dieser Spline.

## Entscheidungen

| Frage | Entscheidung | Grund |
|---|---|---|
| Wo steht die Vergleichsbasis | Kontur am Körper, nicht zweite Rasterebene | klein, auflösungsunabhängig, geht durch Szene und Undo ohne Sonderweg |
| Was heißt „Pinsel-Anteil“ | aus den Zellen abgelesen, nicht mitgeführt | der Pinsel (Schritt 7) bleibt unverändert; es gibt nichts, das aus dem Takt geraten kann |
| Wer löst das Umformen aus | Sync in `updateTerrains`, Fingerabdruck der Spline | geht für Gizmo, Details-Panel, MCP, Landscape verschieben, ohne dass jeder Weg daran denken muss |
| Erster Blick nach Laden/Undo | nur Fingerabdruck merken | Laden und Undo ändern kein Wasser; Gleitkommarauschen zwischen Maschinen formt nichts um |
| Aushub und Umformen | getrennt | verlangt der Plan; ein Bett, das bei jedem Ziehen mitgegraben würde, ließe sich nicht mehr aufräumen |
| Level-Standard | tiefster Boden unter der Kontur (vor dem Graben) | der See liegt im Becken, die höhere Seite wird von der Böschung gefasst |
| Dig Again ohne Änderung | kein Undo-Eintrag | die Welt wird vorher beiseite gelegt und nur bei Änderung als Schritt gebucht; die *Böschung* wird bei jedem Druck etwas tiefer (der Rand wird aus der Höhe von jetzt geblendet), das Bett selbst nicht |
| To Lake, Spline-Eltern | Kind des Landscape, Punkte im Landscape-Raum | wandert mit; keine Matrix-Umrechnung nötig |
| Beschneidung | Landscape-weit, im Lake-Panel gezeigt | die Einstellung sitzt im Netzbau (Schritt 5), „je See“ wäre eine zweite Wahrheit |

## Validierung

- **Datenmodell** (`tests/test_water_lake.cpp`, 33 Fälle): Anlegen, abgelehnte Eingaben (nichts entsteht), tiefster
  Boden, Aushub (nie heben, Böschung, tieferes Loch bleibt), die sechs Folgen der Regel einzeln, Umformen berührt den
  Boden nicht, abgelehntes Umformen lässt die Zellen bit-gleich, Detach, Kontur-Extraktion (Ring, Obergrenze, Inseln,
  mehrere Stücke), `adopt`, Entity-Ebene (`polygonOf` in jedem Raum, `create` mit Aushub, Plan/Apply), Sync (Punkt,
  Entity, Landscape bewegt, kurz nicht geschlossen, Spline gelöscht), **Rundreise Speichern/Laden** (Kontur, Link,
  Pinsel-Anteil überleben, danach formt der Sync weiter um, ohne die Rinne oder die Kerbe zu verlieren) und beschädigte
  Kontur, **Undo** (Wasser *und* Boden bit-genau, auch die Kontur samt Link nach Redo), die Fläche im selben
  `updateTerrains`.
- **Editor-Verdrahtung**: Spline-Panel headless (Create Lake = 1 Schritt, Undo/Redo, Dig Again, Remove Lake, Formular
  erscheint nur bei geschlossener Spline), Wasser-Panel (To Lake = 1 Schritt, Modus-Wechsel, Auswahl, Undo).
- **API/MCP**: 7 + 6 Fälle (Welt-Koordinaten, ein Teich-Strich verlängert den See, Teich → See, neutral ohne Welt,
  Verweigerungen, kein Undo-Eintrag ohne Änderung).
- **Zeuge** `HE_DUMP_LAKEEDIT=drawn|extended|reshaped|converted` (`EditorApplication.cpp`): echte Pfade
  (`lake::create`, `HE::water::brush`, der Sync in `updateTerrains`, `convertBody`), Spline-Hilfslinien darüber.
  Rezept: `HE_CONFIG_DIR=<frisch> HE_COLLAB_OFFLINE=1 HE_SKY_TIME=1.0 scripts/he_shot.py out.png LAKEEDIT=extended
  CAMY=375 CAMZ=48 PITCH=-60 RHI=Metal` (oder `OpenGL`). Die Logzeile ist der Zwilling zum Bild: *drawn* 2564 Zellen,
  4267 Vertices gegraben; *extended* 2771 (die Pinsel-Rinne setzte den See fort); *reshaped* 2771 → 2865, Rinne noch
  nass, Insel noch trocken; *converted* Teich 2287 Zellen → See 2275 Zellen mit 20 Punkten.

OpenGL zeichnet dieselbe Szene (nur die Beleuchtung weicht leicht ab):

![Derselbe See auf OpenGL](water-lake/opengl-extended.png)

## Was dieser Schritt nicht anfasst

D3D11/D3D12/Vulkan (Schritte 9 und 10): die Fläche ist ein gewöhnliches Mesh mit Material, hier nur auf Metal und
OpenGL gesehen. Handbuch-Eintrag und Gesamtabnahme (Schritt 11). Neue Wasser-Shader-Features, Strömung, Auftrieb:
außerhalb des Themas. Nicht gebaut: eine Anzeige der Kontur des Pinsel-Anteils im Viewport; die Spline-Hilfslinien
zeigen die Kontur, der Pinsel-Anteil ist an der Wasserfläche selbst zu sehen.

## Nachtrag Schritt 13: die Böschung grub zu weit, das Wasser blieb zu klein

Rückmeldung mit Screenshot: Beim Lake-Werkzeug (und, über denselben Mechanismus, bei `Dig Again`) grub die Böschung
**nach außen** über die gezeichnete Kontur hinaus — der beschriebene Vertrag oben ("Böschung `Bank` Meter weit nach
außen") war ursächlich korrekt umgesetzt, aber für eine Wasserfläche falsch gewählt: die Böschung sinkt von der
Kontur aus `Bank` Meter weit über das umgebende Gelände hinweg bis knapp an den Wasserspiegel heran (bei flachem
Gelände sogar bis exakt an ihn), aber die Wasserfläche selbst (`water::create`) rastert nur das gezeichnete Polygon,
nicht die Böschung davor. Das Ergebnis: ein **trockener, abgesenkter Ring außerhalb der Spline**, tiefer als der
eigene Wasserspiegel, aber ohne Wasser darüber — sichtbar als freigelegter, zu tief wirkender Aushub, während die
Wasserfläche schon an der Kontur endet, bevor die Böschung überhaupt beginnt.

Die Fläche selbst endete also schon immer exakt an der Kontur (korrekt); die wahrgenommene "Wasser hört zu früh auf"
kam von der sichtbar freigelegten, zu tiefen Böschung direkt daneben — optisch nicht von der Kontur zu unterscheiden,
ohne den Code zu lesen.

**Fix, erster Durchgang:** `TerrainSculpt::ExcavateParams` bekommt `bankIn`/`edgeHeight` (`TerrainSculpt.h`/`.cpp`),
eine zweite, nach INNEN gemessene Böschungszone: `amount` (das Bett) bleibt der Boden ab `bankIn` Meter von der
Kontur nach innen, dazwischen blendet die Höhe smoothstep-artig zu `edgeHeight` (dem Wasserspiegel) genau auf der
Kontur. `apply()` und `dig()` in `WaterLake.cpp` setzten `bankIn = bank`, `edgeHeight = level`, `falloff = 0` —
außerhalb der Kontur sollte nichts mehr angerührt werden.

Vorher/Nachher am selben Fall (`HE_DUMP_LAKEEDIT=drawn`, D3D11, Bohnen-Spline, Tiefe 2,5 m, Bank 6 m, flaches
Gelände): vorher ein sichtbar abgesenkter, beschatteter Ring direkt außerhalb der Kontur (4267 gegrabene
Bodenvertices für 2564 Wasserzellen), nachher kein Ring mehr (2565 gegrabene Bodenvertices, fast genau die
Zellenzahl):

![Vorher: abgesenkter Ring außerhalb der Kontur](water-lake/bugfix-13-before-d3d11.png)
![Nachher: Boden und Wasserkante schließen bündig ab](water-lake/bugfix-13-after-d3d11.png)

**Zweiter Durchgang — der erste Durchgang grub eine Klippe:** `falloff = 0` heißt "außerhalb der Kontur wird
nichts berührt". Auf **flachem** Gelände (der Zeuge oben) fällt das nicht auf, weil der Boden an der Kontur dort
ohnehin schon beim Wasserspiegel liegt. Auf geneigtem oder unebenem Gelände — genau der Fall in den
Original-Screenshots des Menschen — ist der Boden an den meisten Stellen der Kontur HÖHER als der Wasserspiegel
(`level` ist der NIEDRIGSTE Punkt entlang der ganzen Kontur, also liegt der Rest darüber). Innen wird dieser Punkt
auf `edgeHeight` heruntergezogen; einen Gitterschritt weiter außen bleibt der Boden unverändert auf seiner alten,
höheren Höhe — eine Klippe, keine sanfte Böschung. Der Mensch hat genau das über die Queen präzisiert: "am Ufer
soll das Ausheben nicht steil/abrupt runtergehen, sondern eine langsame Abflachung zum Ufer hin stattfinden".

Der Fix: `falloff` bleibt auf `bank` (nicht 0), aber wenn `bankIn > 0` ist, zielt die AUSSEN-Böschung jetzt
ebenfalls auf `edgeHeight` statt auf `amount` (`excavatePolygon`s Außen-Zweig, `TerrainSculpt.cpp`). Beide Seiten
erreichen exakt dieselbe Höhe direkt auf der Kontur, der Boden senkt sich draußen nur dort ab, wo er über dem
Wasserspiegel steht (nie darunter — das Moat-Problem kommt dadurch nicht zurück), und blendet nach `bank` Metern
weich zurück zum unveränderten Gelände. `apply()`/`dig()` in `WaterLake.cpp` setzen `falloff = bank` wieder.

Vorher/Nachher am selben Fall, diesmal über dem WATERLAKE-Zeugen-Hang (`0,12·x + 1,2·sin(0,15·z)`, neuer Modus
`HE_DUMP_LAKEEDIT=slope`): der größte Höhensprung zwischen zwei benachbarten Bodenvertices fällt von 8,89 m (erster
Durchgang, sichtbar gezackt/stufig am Ufer) auf 1,74 m (zweiter Durchgang, durchgehend sanft):

![Vorher: gezackte Klippe am Ufer auf geneigtem Gelände](water-lake/bugfix-13-slope-before-d3d11.png)
![Nachher: durchgehend sanfte Böschung auf beiden Seiten der Kontur](water-lake/bugfix-13-slope-after-d3d11.png)

**Was weiterhin gilt, auch nach dem Fix:** `Floor`-Modus hebt das Gelände nie an. Ein Ring oder eine Klippe, die mit
einem der beiden fehlerhaften Zwischenstände schon in eine bestehende Szene gegraben wurde, bleibt dort liegen —
ein erneutes Create Lake oder Dig Again senkt nur weiter ab, es hebt den alten Fehlgriff nicht wieder an. Betroffene
Seen brauchen Undo (solange die Historie reicht) oder von Hand nachsculpten (Raise/Flatten). Derselbe Mechanismus,
anderer Code, traf auch den Wasser-Pinsel (`TerrainSculpt::lowerToFloor`) — siehe `docs/water-brush-2026-10-09.md`,
Nachtrag Schritt 13 (dort war die Außen-/Innen-Geometrie von Anfang an gekoppelt, kein zweiter Durchgang nötig).

## Nachtrag Thema 188 Schritt 2 (12.10.2026): Das Bett folgt dem gezogenen Punkt

Rückmeldung des Menschen: Beim Lake-Werkzeug zieht ein verschobener Spline-Punkt nur die Wasserfläche mit, der
Aushub bleibt auf der alten Form stehen. **Ursache:** Das war bis hierher Vertrag (`WaterLake.h`, „Moving a spline
point: the WATER only“). `syncSplines` grub nur dann neu, wenn sich die *Transform* des Spline-Entitys änderte
(Schritt 14), und der Bodenpatch, mit dem das alte Bett angehoben wird, war reine Laufzeit: Nach einem Undo, dem
Ende von Play oder einem Laden war er weg, auch das Verschieben des ganzen Sees grub dann nicht mehr mit.

**Jetzt:** Ändert sich die Kontur im Raum des Landscapes (Punkt ziehen, einfügen, löschen, Entity oder Landscape
bewegen) und hat der See sein Bett selbst gegraben (`Body::digsBed`, gesetzt von Create Lake mit Dig Bed und von
Dig Again), hebt `syncSplines` in **jedem Frame des Ziehens** das alte Bett an (`groundPatch` abziehen, additiv, ein
Nachsculpten bleibt) und gräbt mit Tiefe und Böschung des letzten Aushubs ein neues unter der neuen Kontur. Das Flag
statt „Patch nicht leer“: Ein Neu-Graben, das nichts bewegt (Kontur kurz über tieferem Boden oder neben dem
Terrain), lässt einen leeren Patch zurück und darf das Folgen nicht beenden. Dig Again **mischt** seinen Patch in den
des Sees, eine spätere Bewegung hebt also beide Aushübe an. Ein See ohne eigenes Bett (Create Lake ohne Dig Bed,
umgewandelter Teich, Szene von vor Thema 188) formt weiter nur das Wasser um; ein Dig Again macht ihn zum folgenden.

**Leistung beim Ziehen:** Gelesen wird nur das Rechteck, das altes Bett und neuer Aushub erreichen
(`TerrainSculpt::excavateReach`, derselbe Code, den `excavatePolygon` läuft), nicht das ganze Höhengitter; für den
Chunk-Neubau markiert wird nur, wo die Höhe sich *netto* geändert hat. Gemessen im Debug-Build, 1025² Vertices,
See mit 100 m Durchmesser, ein Punkt um 0,25 m je Frame: **1,23 ms je Sync** (schlimmster 1,54 ms), Dirty-Rechteck
höchstens **27 × 70 m** statt der ~110 × 110 m des ganzen Sees (`test_water_lake.cpp`, „re-digs only around that
point“). `syncSplines` läuft jetzt **vor** der Chunk-Schleife in `updateTerrains`, damit der neue Boden im selben
Aufruf gebaut und an die Ufer-Beschneidung übergeben wird, nicht einen Frame später.

**Gespeichert:** `digsBed`, Tiefe, Böschung und Patch stehen am Körper unter `lakeBed` (nur bei grabenden Seen,
alle anderen Szenen bleiben byte-gleich); `sanitize` verwirft einen Patch, der nicht ins Höhengitter passt
(`patchFits`), und das Anheben prüft ihn selbst noch einmal.

Zeuge `HE_DUMP_LAKEEDIT=reshaped` (jetzt mit Bodenhöhen in der Logzeile), Vulkan und D3D11 gleich:

| | Boden unter dem herausgezogenen Lappen (27, −12) | in der hineingedrückten Bucht (4, 16) |
|---|---|---|
| vorher (Neu-Graben abgeschaltet) | 0,00 → 0,00 m (Wasser auf ungegrabenem Boden) | −0,65 → −0,65 m (trockene Grube) |
| nachher | 0,00 → −2,44 m | −0,65 → 0,00 m |

![Vorher: am Südufer bleibt die alte Grube als dunkler, trockener Streifen](water-lake/thema188-redig-before-vulkan.png)
![Nachher (Vulkan): das Bett ist mitgewandert, das Ufer schließt bündig](water-lake/thema188-redig-after-vulkan.png)
![Nachher (D3D11)](water-lake/thema188-redig-after-d3d11.png)

Offen: Das In-Editor-Handbuch (`he-docs.json`, Abschnitt `editor#landscape-mode`, Quelle im Website-Repo) sagt noch
„dragging a point only reshapes the water“; es wird aus dem Website-Checkout neu gebaut, der auf NN-WS03 fehlt.
