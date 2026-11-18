# Landscape 3.0 Folge, Schritt 9: Vorschlaege fuer Steigungs-Uebergaenge und Scattering (2026-11-18)

Thema 329, Schritt 9, **§1 korrigiert in Schritt 10** (Gate-Fund aus Lauf 2530, zwei Punkte:
§1s Ursachenbeleg und die Bildraum-Mockups — siehe §1 und §2). **Nur Vorschlaege, keine
Umsetzung** — auch Schritt 10 aendert weder `AutoLandscapeMaterial.cpp` noch ein Material
dauerhaft: die probeweisen Patches fuer V1/V3 (§2/§3) wurden nach dem Rendern wieder entfernt.
`git diff` gegen den Elternstand zeigt nur `docs/`.

## 1. Ursache des Bug-Reports (Datei 247, `.hive/dateien/247-2.jpg`) — korrigiert (Schritt 10)

**Dieser Abschnitt ersetzt die Fassung aus Schritt 9.** Der Gate-Befund von Lauf 2530 war
berechtigt: die vorherige Fassung belegte ihren Befund nur mit dem Flaeche-Bombing-Paar
(`evidence-flat-bombed.jpg`/`-nobomb.jpg`, von oben, ohne jede Steigung) und schloss von dort
per Ausschlussargument auf die Steilhang-Kante aus `slope-before.jpg`, ohne diese je mit und
ohne Bombing selbst zu rendern. Schritt 10 hat das nachgeholt.

Der Mensch meldet in Foto 247 einen harten, geraden Rand an einem Steilhang, dunkles Gras/Dirt
links gegen helles Fels rechts, als **senkrechte** Kante (oben-unten durch das ganze
Wandstueck), mit der flachen Flaeche *oberhalb und unterhalb* der Wand an derselben
Welt-X-Position durchgehend und ohne Naht. Foto 247 ist aber eine Aufnahme aus einer **echten,
geflogenen** Szene (Hoehe variiert in X **und** Z), nicht vom analytischen Witness-Terrain
dieses Dokuments, das per Konstruktion **nur entlang X** variiert und entlang Z konstant ist
(`EditorApplication.cpp:7857-7864`, `heightScale` je `x`, keine Z-Abhaengigkeit). Eine Kante,
die bei **jeder** Kamerarichtung unabhaengig von Z ist, kann aus `rockT`
(`AutoLandscapeMaterial.cpp:428-430`) auf diesem Terrain gar nicht entstehen — das Terrain
selbst liefert kein Gegenbeispiel, mit dem sich die Steigungsschwelle als Ursache von 247
ausschliessen oder bestaetigen liesse. Die alte Fassung hat das verbal (nicht per Render)
versucht und sich dabei auf ein Bildpaar gestuetzt, das eine ganz andere Kante zeigte.

**Was Schritt 10 tatsaechlich gerendert hat** (`HE_DUMP_AUTOLAND=1|nobomb|masks`, Mountains,
OpenGL, Release, dieselbe Kamera wie `slope-before.jpg` aus Schritt 9, `CAMX=-62 CAMY=326
PITCH=-24 YAW=90`, plus eine um 30° gedrehte Variante `YAW=60`):

- `slope-bomb-full.jpg` / `slope-nobomb-crop.jpg`: bei gleicher Kamera liegt die Rock/Boden-
  Kante an derselben Stelle und in derselber Form, ob Bombing an oder aus ist — gemessen per
  Spalte (Farbheuristik B-Kanal minus R-Kanal, 15-Spalten-Glaettung gegen die gelben
  Platzhalter-Dekals und Pfuetzenschatten, Zeilen 380-470): mittlere Kantenzeilen-Differenz
  4,8 Px, einzelne Ausreisser bis 63 Px (Dekal-/Schattenrauschen der Heuristik, kein
  durchgehender Versatz) — kein "pixelgenau", aber auch keine systematische Verschiebung durch
  Bombing. Nur das Muster *innerhalb* der beiden Flaechen aendert sich (regelmaessige vs.
  unregelmaessige Kachelung, wie im Flaeche-Paar aus §2).
- `slope-masks-crop.jpg`: die unlit-Maskenansicht (R = `r.rockMask`, kein Texturzugriff, kein
  Bombing-Knoten im Graph) zeigt dieselbe Kantenzeile wie die beiden texturierten Bilder, im
  Rahmen derselben Heuristik-Unsicherheit (Textur-Blendzone vs. harte Maskenkante).
- `slope-oblique-bomb.jpg` / `slope-oblique-nobomb.jpg` (YAW=60, zeigt die Kante diagonal statt
  waagerecht): wieder keine erkennbare Verschiebung mit/ohne Bombing.

**Befund (korrigiert):** Die Kante in `slope-before.jpg` ist die **Steigungs-/Hoehen-Schwelle**
`rockT` (`r.slope + heightBias(...)`, `AutoLandscapeMaterial.cpp:428-430`), **nicht** eine
Textur-Bombing-Zellgrenze (Mikkelsen-Hex-Tiling, `MaterialGraph.cpp:2381-2413`,
`heBombGridV`/`heBombTapV`). Bombing bleibt echt und sichtbar — siehe das Flaeche-Paar in §2 —
aendert aber nur die Oberflaechen-STRUKTUR *innerhalb* eines Materials, nicht die GRENZE
zwischen zwei Materialien. Die Vermischungs-Exponent-Beobachtung von Schritt 9 (`sharp` fest auf
`kMatBombDefaultSharpness = 7.0`, `bn->p[1]` nie gesetzt, `AutoLandscapeMaterial.cpp:150-163`)
bleibt technisch richtig, erklaert aber nur das Hex-Mosaik aus §2, nicht die Steilhang-Kante.

**Bombing als Ursache von 247 ebenfalls ausgeschlossen.** `slope-bomb-full.jpg` (YAW=90, dieselbe
Blickrichtung face-on zur Wand wie in 247: Plateau oben, Wand in der Mitte, Ebene unten) zeigt
mit Bombing an **keine** durchgehende, grossraeumige Links/Rechts-Teilung der Wand — nur das
kleinraeumige, unregelmaessige Hex-Mosaik aus §2, pro Hex unabhaengig rotiert/versetzt. Das ist
kein Zufall: jeder Hex-Tap bekommt seinen Versatz unabhaengig von seinen Nachbarn
(`heBombHash`, `MaterialGraph.cpp:2381-2413`) — es gibt keinen gemeinsamen Parameter, der alle
Hexes auf der linken Haelfte einer Wand in die eine und alle auf der rechten Haelfte in die
andere Richtung schieben wuerde. Eine einzelne, gerade, viele Hex-Zellen und die volle
Wandhoehe uebergreifende Trennlinie ist mit diesem Mechanismus strukturell nicht baubar, in
keiner Kamerarichtung. Was durch Bombing offen bleibt, ist nur noch: **was den Rand in 247
tatsaechlich erzeugt** — `rockT` bei real (anders als im analytischen Witness) auch in Z
variierender Steigung/Hoehe, ein Paint-Layer oder eine Gruppen-/Textur-Grenze sind alle
plausibel, aber auf dem Z-konstanten Witness-Terrain dieses Dokuments nicht zu pruefen
(`EditorApplication.cpp:7857-7864`, `heightScale` je `x`, keine Z-Abhaengigkeit). Das braeuchte
eine Kamera auf der echten, geflogenen Szene oder ein Z-variierendes Witness-Terrain — beides
ausserhalb des Umfangs von Schritt 10.

**Ausgeschlossen:** Thema 210 (sichtbare Sechsecke nach verspaetetem Textur-Array-Nachladen,
`docs/landscape-hex-reload-ursache-2026-10-18.md`) ist ein **anderer Fehler** — ein veralteter
Negativ-Cache im Renderer-Resolve, der auf das weisse Fallback-Array bombt. Hier sind die
Arrays bereits gebunden; das Bombing selbst erzeugt die Hex-Textur-Struktur, nicht ein falsches
Array.

**Praezisierung der Queen (request 184) uebernommen:** der Mensch will vor allem die
sichtbaren **sechseckigen Muster auf flacher Flaeche** weg (Vorschlaege 1+2 unten, Ursache
bestaetigt: Bombing). Auf Steigungen muss der Uebergang **sanfter UND aufgebrochen** sein, nicht
optional (Vorschlaege 3+4) — die Ursache dort ist die `rockT`-Schwelle selbst (jetzt belegt,
siehe oben), nicht Bombing; V3/V4 wirken also direkt auf die Schwelle, nicht auf einen
Bombing-Parameter.

## 2. Szene und Kamera (fuer alle Bilder dieselbe Engine-Konfiguration; Kamera je Bild notiert)

Headless-Witness `HE_DUMP_AUTOLAND=<mode>` (analytisches 128-m-Terrain, `EditorApplication.cpp:7734`,
flache Ebene x<-24, Rampe bis ~62 Grad, 40-m-Plateau, Gruppe Mountains Default), echt
geflogen (Release-Build dieses Branches, Commit `bb855163f`, OpenGL, headless
`HE_HIDDEN_WINDOW=1`, Platzhalter-Texturen aus `landscape_tex_gen`, nicht eingecheckt). Schritt
10s Bilder kommen aus demselben Quellstand (kein `src/`-Commit zwischen `bb855163f` und dem
Stand dieses Schritts), gebaut unter `C:/hwLS11`. Die Kamera ist nicht fuer alle Bilder
identisch: §2 nennt die Basis-Kamera je Gruppe (Flaeche/Steilhang), Schritt 10 ergaenzt pro
Gruppe eine zweite, gedrehte Kamera (s.u.) — wo eine Bildunterschrift von der Basis-Kamera
abweicht, steht das dabei. Bilder liegen in `docs/img-landscape-slope-scatter-proposals-2026-11-18/`:

- **Flaeche** (Vorschlaege V1/V2), Kamera `CAMX=-45 CAMY=320 PITCH=-85 YAW=90` (naeherungsweise
  von oben, zoomt auf die flache Ebene): `evidence-flat-bombed.jpg` (`HE_DUMP_AUTOLAND=1`,
  ausgeliefertes Material, Bombing an) gegen `evidence-flat-nobomb.jpg` (`HE_DUMP_AUTOLAND=nobomb`,
  dieselbe Kamera, Bombing aus — die eingebaute Negativkontrolle des Projekts selbst,
  `EditorApplication.cpp:7755-7845`). Dieses Paar belegt, dass Bombing auf flacher Flaeche ein
  sichtbares Hex-Mosaik erzeugt: mit Bombing ein unregelmaessiges, aber klar zellenfoermiges
  Muster (jede der gelben L-Platzhalter-Marken eigens rotiert/versetzt, §6 in
  `auto-landscape-material-textures.md`), ohne Bombing eine stumpf wiederholte, aber regelmaessige
  Kachelung. `evidence-flat-bombed.jpg` ist zugleich das **Vorher**-Bild von V1 und V2. (Schritt 9
  nannte dieses Paar faelschlich auch den Beleg fuer die Steilhang-Kante aus §1 — das Paar zeigt
  keine Steigung und kann das nicht belegen; siehe §1.)
- **Steilhang** (Vorschlaege V3/V4), Kamera `CAMX=-62 CAMY=326 PITCH=-24 YAW=90` (Thema 329
  Schritt 5, `docs/landscape-3-0-rock-by-size-2026-10-30.md:39`), Zeilen 360-520 des Frames
  zugeschnitten (die Rock/Boden-Kante am Rampenfuss): `slope-before.jpg`, **Vorher** fuer
  V3 und V4. Schritt 10 hat dieselbe Kamera zusaetzlich mit `HE_DUMP_AUTOLAND=nobomb` und
  `=masks` gerendert (`slope-bomb-full.jpg`, `slope-nobomb-crop.jpg`, `slope-masks-crop.jpg`) und
  eine um 30° gedrehte Variante (`YAW=60`: `slope-oblique-bomb.jpg`, `slope-oblique-nobomb.jpg`)
  — der empirische Beleg fuer den korrigierten Befund aus §1.

Fuer V3 und V1 hat Schritt 10 die in ihren `Dateien`-Eintraegen beschriebene Graph-Aenderung
**probeweise, uncommitted** in `AutoLandscapeMaterial.cpp` angewandt, den Editor (und fuer V1
zusaetzlich `landscape_tex_gen`) neu gebaut, mit `HE_DUMP_AUTOLAND` echt gerendert und die
Aenderung danach wieder entfernt (`git status` am Ende dieses Schritts zeigt keine Aenderung an
`AutoLandscapeMaterial.cpp` oder einem `.hasset`): `v1-proto-after.jpg` (echtes Rendering,
`bn->p[1] = 2.5f`, verifiziert als `2.500000` im gebackenen GLSL von `M_AutoLandscape.hasset`,
14 Fundstellen) und `v3-proto-masks-after-crop.jpg` + `v3-proto-diff-crop.jpg` (echtes
`=masks`-Rendering mit zusaetzlichem nullmittelwertigem Fbm-Term auf `rockT`, Pixel-Diff gegen
`slope-masks-crop.jpg`: 39020 von 921600 Pixeln aendern sich entlang der Kante). Fuer V2 und V4
blieb es bei den Bildraum-Mockups unten (Zeit-/Baubudget dieses Schritts; siehe die jeweiligen
Abschnitte) — sie sind **keine Engine-Renderings**, sondern aus den Vorher-Pixeln erzeugte
Mockups (ein Skript ausserhalb des Repos, nicht eingecheckt: grossskaliges Rauschfeld als
Helligkeits-Multiplikator fuer V2, ein auf 34-Px-Bloecke quantisierter Zeilen-Versatz pro Spalte
fuer V4) und zeigen nur die **Richtung** der Wirkung, nicht das Shader-Ergebnis; das verbliebene
Mockup (`v4-after.jpg`) traegt diesen Hinweis als eingebrannte Bildunterschrift.

## 3. Vorschlaege

### V1 — Bombing-Schaerfe senken und pro Rolle entkoppeln (flache Flaeche)

**Beschreibung.** Den Bomb-Knoten der gelesenen Rolle (`AutoLandscapeMaterial.cpp:150-163`)
die Pins `Rotation`(p[0])/`Sharpness`(p[1]) explizit setzen statt beim Node-Default zu
bleiben: eine neue `kAutoLandscapeParamBombSharpness`-aehnliche Konstante (kein Pin noetig,
ein fester niedrigerer Wert reicht, z. B. 2.0-3.0 statt 7.0) macht aus der Fast-Stufe eine
echte Dreifach-Ueberblendung ueber die ganze Zelle. Da `heBombGrid` ohnehin schon alle drei
Taps liest (3x Textur-Kosten sind **bereits bezahlt**, §2.2 in `auto-landscape-material-textures.md`),
kostet eine andere Potenz in `pow(w, sharp)` nichts zusaetzliches.
**Bild.** `v1-flat-baseline.jpg` (Schritt 10, derselbe Lauf, Original-Schaerfe 7.0, **nicht**
`evidence-flat-bombed.jpg` — das ist ein JPEG aus Schritt 9 mit anderem GL-Programm-Binary-Cache-
Zustand und daher kein fairer Pixel-Vergleich, siehe unten) / `v1-proto-after.jpg` (Schritt 10,
echtes Rendering derselben Kamera, `bn->p[1] = 2.5f` probeweise gesetzt, Patch danach entfernt).
Beide BMPs stammen aus demselben Lauf; der Diff zeigt 11,4 % der Pixel um mehr als 15/255
veraendert (Mittelwert 2,24/255, Maximum 103/255), raeumlich an den Hex-Kanten ausgerichtet
(`v1-vs-baseline-diff.jpg`, nicht eingecheckt) — ein schmalerer/breiterer Blend-Saum, kein
globaler Effekt, auf dem kontrastarmen Platzhalter mit blossem Auge kaum zu sehen. Zusaetzlich
direkt im gebackenen GLSL verifiziert: `heBombGridV(..., 2.500000, ...)` an allen 14
Aufrufstellen statt `7.000000`. **Cache-Hinweis:** `V1-flat-baseline.log` zeigt "loaded a
material program from the on-disk binary cache", `V1-flat-mode1.log` (die gepatchte Variante)
zeigt "built a CLUSTERED material program ... from canonical GLSL" — beide Werte sind also
unabhaengig von diesem Cache tatsaechlich unterschiedlich kompiliert und gerendert worden
(siehe `gl-program-binary-cache-first-run-skew` in den Projekt-Notizen), ein kontrastreicheres
Echtfoto-Material duerfte den Effekt deutlicher zeigen (siehe Risiko).
**Kosten.** ~0 ALU zusaetzlich (eine andere Konstante im selben `pow`); 0 neue Texturzugriffe.
Nicht gemessen (kein Perf-Capture in diesem Schritt, nur Korrektheit der Shader-Aenderung).
**Risiko.** Niedrig fuer die Kosten, mittel fuer den Look: zu niedrige Schaerfe zeigt an der
Zellgrenze kurz zwei leicht verschobene/rotierte Kopien derselben Textur gleichzeitig
("Geister"-Doppelbild) statt einer sauberen Mischung — ein Mittelwert muss am echten Rock-Foto
getestet werden, nicht nur am kontrastarmen Platzhalter (`v1-proto-after.jpg` bestaetigt genau
diese Einschraenkung: der Effekt ist auf dem Platzhalter kaum zu sehen).
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
**Bild.** `evidence-flat-bombed.jpg` (Vorher, dasselbe wie bei V1) / `v2-after.jpg` (weiterhin
ein Bildraum-Mockup, kein Engine-Rendering — in diesem Schritt nicht nachgeholt, siehe §2:
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
**Bild.** `slope-masks-crop.jpg` (Vorher, `=masks`-Witness, dieselbe Kamera wie
`slope-before.jpg`) / `v3-proto-masks-after-crop.jpg` (Schritt 10, echtes `=masks`-Rendering mit
dem im Beschreibungstext genannten Fbm-Term probeweise auf `rockT` addiert, Patch danach
entfernt) und `v3-proto-diff-crop.jpg` (Pixel-Diff der beiden: 39020 von 921600 Pixeln, ein
durchgehendes Band entlang der alten Kante — die Schwelle bricht sichtbar auf, genau wie
beschrieben, nicht als Mockup-Simulation sondern als echtes Shader-Ergebnis).
**Kosten.** +1 `Fbm` + 1-2 ALU (Multiply, Add) in Spalte 4, nur dort wo `rockCan` sowieso
schon gilt (die `If`-Gates bleiben). Keine neue Textur. Nicht gemessen (kein Perf-Capture in
diesem Schritt, nur Korrektheit der Shader-Aenderung).
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
**Bild.** `slope-masks-crop.jpg` (Vorher, dasselbe wie bei V3) / `v4-after.jpg` (weiterhin ein
Bildraum-Mockup, kein Engine-Rendering — in diesem Schritt nicht nachgeholt, siehe §2: dieselbe
Kante, aber die Verschiebung ist auf ein grobes Gitter quantisiert statt glatt).
**Kosten.** +1 `heBombCells`-Aufruf (Integer-Hash, kein Texturzugriff) + 1-2 ALU. Guenstiger
als V3s Fbm (kein Gradient-Rauschen), aber ein zusaetzlicher Hex-Lookup, der mit einer
**anderen** Zellgrosse laufen sollte als das Textur-Bombing selbst — sonst fallen die
Facetten-Kanten der Schwelle exakt mit den (durch V1 ohnehin schon abgeschwaechten) Textur-
Hex-Kanten zusammen und das **sichtbare Hex-Mosaik aus §2** (nicht die 247-Kante aus §1, die
nicht von Bombing kommt) kommt an der Fels-Schwelle in neuer Form zurueck. Nicht gemessen.
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
den gewaehlten dann **committed** in `AutoLandscapeMaterial.cpp` um, mit Metal/GL-Vergleich
und, falls V1 gewaehlt wird, einem Blick auf die echten (nicht nur Platzhalter-)Rock-Texturen
aus Schritt 6. Fuer V1 und V3 existiert bereits ein echter (uncommitted, probeweise gerenderter
und wieder entfernter) Vorher/Nachher-Witness aus Schritt 10 (§2/§3); V2 und V4 haben das noch
vor sich — nur Bildraum-Mockups bisher. Ob die Z-unabhaengige Kante aus Foto 247 ueberhaupt von
einem dieser vier Vorschlaege behoben wird, bleibt unklar, solange §1s offene Frage (Ursache von
247 selbst) nicht auf der echten, geflogenen Szene oder einem Z-variierenden Witness-Terrain
geklaert ist.
