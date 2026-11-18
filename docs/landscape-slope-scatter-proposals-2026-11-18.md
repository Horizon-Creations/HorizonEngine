# Landscape 3.0 Folge, Schritt 9: Vorschlaege fuer Steigungs-Uebergaenge und Scattering (2026-11-18)

Thema 329, Schritt 9. **Nur Vorschlaege, keine Umsetzung** — dieser Schritt aendert weder
`AutoLandscapeMaterial.cpp` noch ein Material. `git diff` zeigt nur `docs/`.

## 1. Ursache des Bug-Reports (Datei 247, `.hive/dateien/247-2.jpg`)

Der Mensch meldet einen harten, geraden Rand an einem Steilhang, dunkles Gras/Dirt links
gegen helles Fels rechts. Das Bild zeigt eine **senkrechte** Kante (oben-unten durch das
ganze Wandstueck), nicht eine waagerechte, konturfolgende. Entscheidend: die flache
Flaeche *oberhalb und unterhalb* der Wand läuft an genau derselben Welt-X-Position
**durchgehend und ohne Naht** weiter. Eine Kachel-, Chunk-, Gruppen- oder RVT-Page-Grenze
würde auch auf der flachen Flaeche an dieser Stelle sichtbar sein — ist sie aber nicht.
Eine reine Steigungs-/Hoehen-Schwelle (`r.rockMask`, `r.dirtMask`,
`AutoLandscapeMaterial.cpp:378-430`) haengt nur von Steigung und Hoehe ab, die sich entlang
einer geraden Wand mit gleichbleibendem Querschnitt kaum mit X aendern — sie koennte also
auch keine X-abhaengige Kante erzeugen.

**Befund:** Die Kante ist eine **Textur-Bombing-Zellgrenze** (Mikkelsen-Hex-Tiling,
`MaterialGraph.cpp:2381-2413`, `heBombGrid`/`heBombTap`). Jede der drei Hex-Ecken unter
einem Texel bekommt einen eigenen zufaelligen Versatz/Rotation, aber die drei Taps werden
nur ueber eine **Baryzentrik hoch Exponent `sharp`** gemischt (`MaterialGraph.cpp:2412`,
`w = pow(..., sharp); w /= sum`). `sharp` ist **fest auf `kMatBombDefaultSharpness = 7.0`**
(`MaterialGraph.h:269`) und wird in `AutoLandscapeMaterial.cpp`s gebombter Textur-Lese-Lambda
(Zeile 150-163) **nie gesetzt** — nur der Seed (`bn->p[2]`) wird pro Rolle gewaehlt, Rotation
(`p[0]`) und Schaerfe (`p[1]`) bleiben ueberall beim MaterialGraph-Knoten-Default
(`MaterialGraph.cpp:494`). Mit Exponent 7 ist der Uebergang zwischen zwei Hexes schon auf
einem Bruchteil der Zellgrosse (`kAutoLandscapeParamBombCell` Default 0.5 Texturwiederholungen,
Zeile 88-89) fast fertig geblendet — das Ergebnis ist optisch ein harter Rand **genau an der
Hex-Kante**, deren Geometrie (gerade Sechseckseiten) exakt zu einer geraden, X-unabhaengigen
Linie passt. Auf dem Fels-Rolle trifft es besonders hart, weil Fels zusaetzlich drei
Groessen-Varianten ueber Slope blendet (`group.rockBySize`, Zeilen 240-250, 387-427) und jede
eigene Kachelgrosse eine andere Hex-Zellgrosse in Weltkoordinaten ergibt.

**Ausgeschlossen:** Thema 210 (sichtbare Sechsecke nach verspaetetem Textur-Array-Nachladen,
`docs/landscape-hex-reload-ursache-2026-10-18.md`) ist ein **anderer Fehler** — ein veralteter
Negativ-Cache im Renderer-Resolve, der auf das weisse Fallback-Array bombt. Hier sind die
Arrays bereits gebunden; das Bombing selbst erzeugt die Kante, nicht ein falsches Array.

**Praezisierung der Queen (request 184) uebernommen:** der Mensch will vor allem die
sichtbaren **sechseckigen Muster auf flacher Flaeche** weg (Vorschlaege 1+2 unten), und auf
Steigungen muss der Uebergang **sanfter UND aufgebrochen** sein, nicht optional (Vorschlaege
3+4).

## 2. Szene und Kamera (fuer alle Bilder gleich)

Headless-Witness `HE_DUMP_AUTOLAND=<mode>` (analytisches 128-m-Terrain, `EditorApplication.cpp:7734`,
flache Ebene x<-24, Rampe bis ~62 Grad, 40-m-Plateau, Gruppe Mountains Default), echt
geflogen (Release-Build dieses Branches, Commit `bb855163f`, OpenGL, headless
`HE_HIDDEN_WINDOW=1`, Platzhalter-Texturen aus `landscape_tex_gen`, nicht eingecheckt).
Bilder liegen in `docs/img-landscape-slope-scatter-proposals-2026-11-18/`:

- **Flaeche** (Vorschlaege V1/V2), Kamera `CAMX=-45 CAMY=320 PITCH=-85 YAW=90` (naeherungsweise
  von oben, zoomt auf die flache Ebene): `evidence-flat-bombed.jpg` (`HE_DUMP_AUTOLAND=1`,
  ausgeliefertes Material, Bombing an) gegen `evidence-flat-nobomb.jpg` (`HE_DUMP_AUTOLAND=nobomb`,
  dieselbe Kamera, Bombing aus — die eingebaute Negativkontrolle des Projekts selbst,
  `EditorApplication.cpp:7744`). Dieses Paar ist der empirische Beleg fuer den Befund aus §1:
  mit Bombing ein unregelmaessiges, aber klar zellenfoermiges Mosaik (jede der gelben
  L-Platzhalter-Marken eigens rotiert/versetzt, §6 in `auto-landscape-material-textures.md`),
  ohne Bombing eine stumpf wiederholte, aber regelmaessige Kachelung. `evidence-flat-bombed.jpg`
  ist zugleich das **Vorher**-Bild von V1 und V2.
- **Steilhang** (Vorschlaege V3/V4), Kamera `CAMX=-62 CAMY=326 PITCH=-24 YAW=90` (Thema 329
  Schritt 5, `docs/landscape-3-0-rock-by-size-2026-10-30.md:39`), Zeilen 360-520 des Frames
  zugeschnitten (die Rock/Boden-Kante am Rampenfuss): `slope-before.jpg`, **Vorher** fuer
  V3 und V4.

Die **Nachher**-Bilder der vier Vorschlaege (`v1-after.jpg` .. `v4-after.jpg`) sind **keine
Engine-Renderings**: dieser Schritt darf `AutoLandscapeMaterial.cpp` nicht aendern. Es sind
Bildraum-Mockups, die aus genau denselben Pixeln der beiden Vorher-Bilder erzeugt wurden (ein
Skript ausserhalb des Repos, nicht eingecheckt: kantenbewusste Weichzeichnung fuer V1,
grossskaliges Rauschfeld als Helligkeits-Multiplikator fuer V2, ein Zeilen-Versatz pro Spalte
— glatt fuer V3, auf 34-Px-Bloecke quantisiert fuer V4 — fuer die Kante bei V3/V4). Sie zeigen
die **Richtung** der Wirkung auf derselben Kamera und Szene, nicht das exakte
Shader-Ergebnis; jedes Bild traegt diesen Hinweis auch als eingebrannte Bildunterschrift.

## 3. Vorschlaege

### V1 — Bombing-Schaerfe senken und pro Rolle entkoppeln (flache Flaeche)

**Beschreibung.** Den Bomb-Knoten der gelesenen Rolle (`AutoLandscapeMaterial.cpp:150-163`)
die Pins `Rotation`(p[0])/`Sharpness`(p[1]) explizit setzen statt beim Node-Default zu
bleiben: eine neue `kAutoLandscapeParamBombSharpness`-aehnliche Konstante (kein Pin noetig,
ein fester niedrigerer Wert reicht, z. B. 2.0-3.0 statt 7.0) macht aus der Fast-Stufe eine
echte Dreifach-Ueberblendung ueber die ganze Zelle. Da `heBombGrid` ohnehin schon alle drei
Taps liest (3x Textur-Kosten sind **bereits bezahlt**, §2.2 in `auto-landscape-material-textures.md`),
kostet eine andere Potenz in `pow(w, sharp)` nichts zusaetzliches.
**Bild.** `evidence-flat-bombed.jpg` (Vorher) / `v1-after.jpg` (Mockup: kantenbewusste
Weichzeichnung, staerker genau dort wo der Gradient zwischen zwei Hexes hoch ist,
Textur-Detail in den Zellinnern bleibt scharf).
**Kosten.** ~0 ALU zusaetzlich (eine andere Konstante im selben `pow`); 0 neue Texturzugriffe.
Nicht gemessen (keine Shader-Aenderung in diesem Schritt).
**Risiko.** Niedrig fuer die Kosten, mittel fuer den Look: zu niedrige Schaerfe zeigt an der
Zellgrenze kurz zwei leicht verschobene/rotierte Kopien derselben Textur gleichzeitig
("Geister"-Doppelbild) statt einer sauberen Mischung — ein Mittelwert muss am echten Rock-Foto
getestet werden, nicht nur am kontrastreichen Platzhalter.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (der `read`-Lambda,
Zeile ~142-164, `bn->p[1]` setzen). Kein neuer Knotentyp, `MaterialGraph.cpp` unangetastet.

### V2 — Grossraeumige Clusterung ueber dem Bombing (flache Flaeche, Scattering)

**Beschreibung.** Ein zweites, viel groesseres Rauschfeld (wiederverwendet: derselbe
`T::Fbm`-Knotentyp wie `dirtFbm`/`basin`, `AutoLandscapeMaterial.cpp:361`, `529`) moduliert
die Albedo-Helligkeit/den Mix zwischen zwei Bombing-Durchgaengen mit **deutlich groesserer**
Wellenlaenge als eine Hex-Zelle (z. B. 15-20x `pCell`). Statt lauter gleich grosser,
isolierter Hex-Flecken entsteht ein organisches Muster aus wenigen groesseren, unregelmaessig
geformten und unterschiedlich grossen Flecken — genau die "Groessenverteilung, Clusterung,
Zusammenhang" aus der Aufgabe, ohne dass die Fels-/Gras-Verteilung selbst (Slope/Height)
angefasst wird.
**Bild.** `evidence-flat-bombed.jpg` (Vorher, dasselbe wie bei V1) / `v2-after.jpg` (Mockup:
grossskaliges, weichgezeichnetes Rauschfeld multipliziert auf die Helligkeit des Originalbilds).
**Kosten.** +1 `Fbm`-Auswertung und 1-2 ALU (Multiply/Lerp) pro Pixel, pro betroffener Rolle.
Keine neue Textur. Nicht gemessen; in derselben Groessenordnung wie `dirtFbm`, das bereits
pro Pixel laeuft.
**Risiko.** Niedrig-mittel: ein zu starker Effekt kann wie ein grossflaechiger Schatten
wirken statt wie Materialvariation; das Rauschfeld braucht einen eigenen, von `dirtFbm`
dekorrelierten Offset (wie `basin` ihn schon von `dirtFbm` hat, Zeile 522-524), sonst
ueberlagern sich zwei Muster sichtbar in Phase.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (neuer `Fbm`-Knoten +
Multiply in der `layer`/`surfaceOf`-Pipeline oder nach dem finalen Blend). Kein neuer
Knotentyp.

### V3 — Rauschen auf die Steigungsschwelle (Steilhang, Muss-Anforderung)

**Beschreibung.** `rockT` (`AutoLandscapeMaterial.cpp:428-430`, heute
`r.slope + heightBias(...)`) bekommt einen dritten, **nullmittelwertigen** Fbm-Term in
Weltraum-XZ addiert (derselbe Aufbau wie `dirtFbm`, aber eigener Seed/Offset), mit kleiner
Amplitude relativ zu `pRockBlend`. Der Effekt: auf einer durchgehend steilen, gleichfoermigen
Wand (konstante Slope ueber die ganze Flaeche) wackelt die Schwelle selbst raeumlich, sodass
Fels/Boden nicht mehr als eine einzige harte Bandgrenze, sondern als aufgebrochener,
organischer Saum erscheinen — genau das, was als Muss-Anforderung gilt ("eine Klippe nicht
durchgehend dasselbe Material"). Die alten Endwerte (ganz flach / ganz steil) aendern sich
nicht, weil das Rauschen nullmittelwertig ist und nur in der schmalen Blend-Zone wirkt.
**Bild.** `slope-before.jpg` / `v3-after.jpg` (Mockup: die erkannte Kante wird entlang ihrer
Laengsrichtung mit einer glatten, niederfrequenten Sinus-/Rauschfunktion seitlich verschoben —
simuliert eine durchgehend wellige, keine stufige Aufbrechung).
**Kosten.** +1 `Fbm` + 1-2 ALU (Multiply, Add) in Spalte 4, nur dort wo `rockCan` sowieso
schon gilt (die `If`-Gates bleiben). Keine neue Textur. Nicht gemessen.
**Risiko.** Niedrig: reine Erweiterung einer bestehenden Formel mit einem bestehenden
Knotentyp, kein neuer Parameter zwingend noetig (Amplitude kann fest an `pRockBlend` gekoppelt
werden). Zu tunen: Amplitude/Frequenz, damit die Automatik nicht insgesamt "fleckiger" wirkt
als gewollt, und dass sie mit `group.rockBySize`s drei Groessen-Rampen (Zeilen 400-420)
zusammenspielt statt sie zu verdoppeln.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile ~428-430, plus
ggf. dieselbe Idee auf `dirtBelt`/`beltE0`, Zeile 371-381, fuer dieselbe Konsistenz zwischen
Boden- und Fels-Kante).

### V4 — Pro-Hex-Schwellen-Jitter ueber die vorhandenen Bombing-Zellen (Steilhang, Alternative/Ergaenzung zu V3)

**Beschreibung.** Statt eines kontinuierlichen Rauschfelds (V3) liest man den **bereits
vorhandenen** `MatNodeType::BombCells`-Knoten (`MaterialGraph.cpp:271-274`, "die Hex-Zelle als
Zahl, ohne Textur") auf demselben Gitter wie die Fels-/Boden-Textur-Bombing-Taps und addiert
dessen Zufallswert (klein skaliert) auf `rockT`. Jede Hex-Zelle bekommt dann einen **festen**,
aber zwischen Nachbarzellen unabhaengigen Schwellen-Versatz: die Kante bricht in facettierte,
zur Zelle passende Stufen statt in eine glatte Welle (V3) — teurer an Gestaltungsfreiheit,
aber **billiger im Shader**, weil `BombCells` nur einen Integer-Hash rechnet, kein Rausch-Gradient.
**Bild.** `slope-before.jpg` (Vorher, dasselbe wie bei V3) / `v4-after.jpg` (Mockup: dieselbe
Kante, aber die Verschiebung ist auf ein grobes Gitter quantisiert statt glatt).
**Kosten.** +1 `heBombCells`-Aufruf (Integer-Hash, kein Texturzugriff) + 1-2 ALU. Guenstiger
als V3s Fbm (kein Gradient-Rauschen), aber ein zusaetzlicher Hex-Lookup, der mit einer
**anderen** Zellgrosse laufen sollte als das Textur-Bombing selbst — sonst fallen die
Facetten-Kanten der Schwelle exakt mit den (durch V1 ohnehin schon abgeschwaechten) Textur-
Hex-Kanten zusammen und das Problem aus §1 kommt in neuer Form zurueck. Nicht gemessen.
**Risiko.** Mittel: die bewusste Spannung zu V1/V2 (keine sichtbaren Sechsecke mehr wollen,
aber hier gezielt ein Sechseckraster als Bruchmuster einsetzen) muss im Tuning sauber
getrennt werden (eigene, groessere Zellgrosse + die aus V1 niedrigere Bombing-Schaerfe).
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (`rockT`, Zeile
428-430; `BombCells`-Knoten neu verdrahten, kein neuer Knotentyp in `MaterialGraph.cpp`).

## 4. Randbedingungen, eingehalten

- **R-1 (Textur-Cap 4 je Graph, vorhandene Slots wiederverwenden):** keiner der vier
  Vorschlaege liest eine neue Textur. V1/V4 aendern nur Zahlenwerte/Pins bestehender
  Bombing-Knoten, V2/V3 nutzen den bereits im Graph vorhandenen `Fbm`-Knotentyp
  (`dirtFbm`/`basin`-Muster) bzw. den bereits registrierten, aber in diesem Material noch
  ungenutzten `BombCells`-Knoten. `heTexP0..3` bleiben bei drei Arrays + Path-Albedo.
- **Alte Szenen laden unveraendert:** keiner der Vorschlaege aendert das Szenen- oder
  Material-Asset-Format; alle vier sind Aenderungen an der **Graph-Konstruktion** in
  `buildAutoLandscapeGraph`, die beim naechsten Laden ohnehin aus dem Graph neu erzeugt wird
  (`ContentManager::regenerateMaterialFromGraph`, wie jede bisherige Aenderung an dieser
  Datei in den Schritten 1-7). Keiner fuegt einen neuen Paint-Layer, Parameter-Slot oder
  Weightmap-Kanal hinzu.
- **Nicht in diesem Schritt:** keine Umsetzung, keine neuen Layer/Texturen, kein
  Schnee-/Sand-Uebergang, kein RVT-Umbau. `AutoLandscapeMaterial.cpp` und die `.hasset`-
  Materialien sind in diesem Diff unveraendert — nur `docs/` und die Bilder darin.

## 5. Naechster Schritt (auf Nachfrage)

Der Mensch waehlt einen oder mehrere der vier Vorschlaege; ein eigener Schritt setzt genau
den gewaehlten dann in `AutoLandscapeMaterial.cpp` um, mit echtem Vorher/Nachher-Witness auf
derselben Kamera (nicht mehr das Bildraum-Mockup), Metal/GL-Vergleich und, falls V1 gewaehlt
wird, einem Blick auf die echten (nicht nur Platzhalter-)Rock-Texturen aus Schritt 6.
