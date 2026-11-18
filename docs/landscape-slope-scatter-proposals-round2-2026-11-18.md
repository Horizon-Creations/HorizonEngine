# Landscape 3.0 Folge, Schritt 11: zweite Vorschlagsrunde — Steigungs-Uebergaenge weich blenden, Dirt nur fuer leichte Steigung (2026-11-18)

Thema 329, Schritt 11. **Nur Vorschlaege, keine Umsetzung** — `git diff origin/claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate` zeigt nur `docs/`. Baut auf
`docs/landscape-slope-scatter-proposals-2026-11-18.md` (Schritt 9) auf; V1/V2 (Hex-Muster auf
flacher Flaeche) sind aus Runde 1 angenommen und werden hier nicht neu bewertet.

## 1. Rueckmeldung des Menschen, uebernommen

Runde 1s "Nachher"-Bilder fuer V3/V4 waren Bildraum-Mockups (ein Zeilen-Versatz pro Spalte) —
das sah wie senkrechte, hoch-runter laufende Streifen aus und war nicht gemeint. Richtig
verstanden:

- Der SHADER selbst aendert sich. Heute malt `AutoLandscapeMaterial.cpp` an einer Steigung einen
  Streifen, sobald die Steigung einen festen Schwellwert ueberschreitet (`r.rockMask =
  ramp(pRockSlope, pRockBlend, rockT)`, Zeile 428-430), und auf Mountains je Rock-Groesse einen
  weiteren (`tMid`/`tBig`, Zeile 404-405) — starre Ringe entlang der Hoehenlinien, weil Steigung
  auf einem gleichfoermigen Hang kaum mit der Horizontalen (Welt-X/Z) variiert. Die Loesung ist
  **nicht** ein breiterer fester Streifen, sondern ein breiter, aufgebrochener Verlauf: die
  Uebergangszone selbst muss quer zur Hangrichtung (als Funktion von Welt-XZ, nicht nur von der
  Steigung) variieren, damit die Kante nicht mehr einer einzigen Iso-Steigungs-Linie folgt. Das
  Muster flach/steiler/sehr steil bleibt, nur die UEBERGAENGE aendern sich.
- Keine Rauschkarte mehr fuer die automatische Dirt-Mischung. Drei Lesarten von "Map", alle
  betroffen:
  1. `dirtFbm`/`dirtNoise`/`dirtT` (Zeile 360-370, 378-380): die Fbm-Rauschflecken auf der
     flachen Flaeche — die woertlichste Lesart.
  2. `dirtBelt` (Zeile 371, 381): **kein** Rauschen, sondern ein reiner Steigungs-Gürtel direkt
     unter der Rock-Schwelle (`beltE0 = pRockSlope − pRockBlend`, bei Mountains-Defaults 0, deckt
     also fast den GANZEN unteren Hang ab) — da der Mensch beide in einem Atemzug nennt, zaehlt er
     hier mit.
  3. Der Hoehenkarten-Bias in `dirtT` (`heightBias(dirt.height, grass.height, …)`, Zeile 378): ein
     photografischer Detail-Bias auf die PLATZIERUNG, keine prozedurale Rauschkarte im engeren
     Sinn, aber auch er entscheidet heute mit, wo Dirt automatisch erscheint.
  Alle drei Vorschlaege unten entfernen alle drei: die neue Dirt-Maske liest nur noch `r.slope`,
  kein Fbm, kein Hoehen-Bias. (Der generische, hoehenkarten-gefuehrte Blend INNERHALB von
  `blend()` — das Lerp, das jede Rolle, auch Rock und Snow, mit ihrer eigenen Hoehenkarte
  einfaerbt — ist davon nicht betroffen: das ist Oberflaechendetail, keine Dirt-Platzierung.)
- Dirt dient nur noch fuer GANZ LEICHTE Steigung, weich mit Gras geblendet, nicht mehr als
  flaechendeckende Automatik auf der flachen Ebene und nicht mehr als Guertel direkt unter dem
  Fels.

## 2. Szene, Kamera, Methode (fuer alle Bilder gleich)

Gleiche Szene wie Runde 1: `HE_DUMP_AUTOLAND` (analytisches 128-m-Terrain, flache Flaeche x<-24,
Rampe bis ~62 Grad auf 32 m, 40-m-Plateau, Gruppe Mountains Default — **konstant entlang Z**, d.h.
jede Z-Variation im Bild kommt einzig vom hinzugefuegten Breakup-Term, nicht von der Geometrie).
Gleiche Kamera wie Runde 1s Steilhang-Paar: `HE_DUMP_CAMX=-62 HE_DUMP_CAMY=326 HE_DUMP_PITCH=-24
HE_DUMP_YAW=90`, `HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.4 HE_DUMP_AA=0 HE_DUMP_RENDERPATH=0
HE_DUMP_RHI=OpenGL`, Release-Build dieses Branches (Commit `5bc88417d`, `C:/hwLS11`, `-j8`,
`HE_HIDDEN_WINDOW=1 HE_COLLAB_OFFLINE=1`, privates `APPDATA`).

**Anders als Runde 1: ECHTE Engine-Renderings, zwei Masken-Witness-Modi statt des lit/Default-Modus.**
`HE_DUMP_AUTOLAND=masks` (R=Rock-Maske `r.rockMask`, G=Schnee, B=Wasser) und `=ground` (R=Dirt-Maske
`r.dirtMask`, G=nass, B=flach) bauen den Graphen **live** aus `HE::buildAutoLandscapeGraph`
(`EditorApplication.cpp:7807-7821`) statt die gebackene `M_AutoLandscape.hasset` zu laden — jede
Aenderung an `AutoLandscapeMaterial.cpp` ist damit sofort im Bild, ohne den `landscape_tex_gen
--material`-Umweg und ohne eine Asset-Datei anzufassen. Das ist zugleich die direkteste Evidenz:
die Bilder zeigen exakt die Maske, die jeder Vorschlag aendert, nicht Licht/Textur-Rauschen obendrauf.
Fuer jedes Bild: `HE_DUMP_PATH=<datei>.bmp HE_DUMP_QUIT=1` plus die Variablen oben, dann BMP zu PNG/JPG.
Die untere Bildhaelfte (Zeile 380-490 von 720, hier als Ausschnitt 3-fach vergroessert gezeigt) ist
der Gras/Dirt-gegen-Fels-Rand am Rampenfuss — derselbe Bildbereich, den Runde 1 als "Vorher" zeigte.

Konsequenz des Masken-Modus: er zeigt `r.rockMask`/`r.dirtMask` selbst, nicht die Textur-Wahl
innerhalb von Rock-by-Size (klein/mittel/gross, `tMid`/`tBig`). Vorschlag D unten (die Baender
DIESER drei Groessen) ist deshalb **nicht gerendert** — siehe dort.

- `slope-before-masks.jpg` / `slope-before-ground.jpg`: **Vorher**, unveraenderter HEAD.
- `v-a-rock-after-masks.jpg`, `v-b-rock-after-masks.jpg`, `v-c-rock-after-masks.jpg`: **Nachher**
  der drei Fels-Uebergangs-Vorschlaege (masks-Modus; jeweils ein eigener temporaerer Patch + Build,
  danach `git checkout --`).
- `v-abc-dirt-after-ground.jpg`: **Nachher** der (bei A/B/C identischen) Dirt-Reform (ground-Modus;
  aus Vorschlag A's Build, da die Dirt-Formel in A/B/C gleich ist).

Jeder Patch wurde nur fuer das Rendern angewendet und direkt danach mit `git checkout --
src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` zurueckgesetzt; `git status`/`git diff`
sind nach jedem Schritt und am Ende leer bis auf `docs/`.

## 3. Gemeinsame Dirt-Reform (alle drei Vorschlaege A/B/C)

**Formel.** Ersetzt den kompletten Block Zeile 360-383 durch eine einzige Rampe ueber die Steigung
allein, ohne Rueckfall auf Gras (kein zweiter Rand): `r.dirtMask = smoothstep(0, pDirtLightSlope,
r.slope)` mit `pDirtLightSlope = 0.03` (~14°, "ganz leichte Steigung"). `ground = blend(grass,
dirt, r.dirtMask)` bleibt die einzige Blendstufe — Mischreihenfolge **Gras → Dirt**, unveraendert
danach von Fels ueberdeckt (Mischreihenfolge **(Gras→Dirt) → Fels**, siehe §4). Die Maske steigt
monoton von 0 auf 1 und faellt NICHT wieder auf Gras zurueck, bevor Fels uebernimmt — ein
Zurueckfallen auf Gras vor der Fels-Schwelle wuerde einen DRITTEN Rand erzeugen
(Gras→Dirt→Gras→Fels), also genau das Problem, das abgestellt werden soll. "Nur fuer ganz leichte
Steigung" bezieht sich auf den UEBERGANG (wo Gras zu Dirt wird), nicht darauf, dass Dirt danach
wieder verschwindet — auf dem sichtbaren Rest des Hangs deckt Fels Dirt ohnehin zunehmend ab
(bei den Werten aus §4 ab ~20°, vollstaendig ab ~46°).

**Bild.** `slope-before-ground.jpg` (Vorher: Dirt flutet fast den GANZEN unteren Hang als
`dirtBelt`, zusaetzlich gesprenkelte Rauschflecken auf der flachen Flaeche unten links/Mitte —
die magenta/tuerkisen Flecken) gegen `v-abc-dirt-after-ground.jpg` (Nachher: die flache Flaeche
ist durchgehend reines Blau = `r.dirtMask` = 0, keine Flecken mehr; auf dem Hang bleibt die
RAW-Maske zwar rot, weil sie oberhalb von `pDirtLightSlope` einfach bei 1 bleibt — das ist die
Kehrseite "kein Rueckfall auf Gras" oben; die tuerkisen Flecken (Pfuetzen, G=nass) sind in beiden
Bildern gleich, wie erwartet, da Vorschlag an Pfuetzen nichts aendert).
**Kosten.** Entfernt 1 `Fbm`-Auswertung (`dirtFbm`) + ~8 ALU-Operationen (Divide, 2x Subtract,
2x Multiply, Add, 2x Ramp/Smoothstep, Saturate), ersetzt durch 1 Ramp (2 ALU). Strikt GUENSTIGER
als heute, nicht teurer — und der entfernte `dirtFbm` lief vorher ueber die ganze flache Flaeche,
waehrend die neue Rampe nur im schmalen Uebergangsstreifen ueberhaupt von 0 verschieden ist.
**Risiko.** Niedrig fuer die Mechanik, mittel fuer den Look: `DirtAmount`/`DirtSize`
(`kAutoLandscapeParamDirtAmount/Size`) werden UNBENUTZT (Parameterknoten bleiben deklariert,
Default unveraendert — alte Szenen und `tests/test_landscape_groups.cpp:203`, das den Default
gegen `group.dirtAmount` prueft, bleiben gruen). `src/HE_Rendering/src/GiAutoLandscape.cpp:39`
liest `kAutoLandscapeParamDirtAmount` fuer die GI-Spiegelung — die Lookup bleibt gueltig (Knoten
existiert), aber ihr heutiger Sinn ("Anteil Dirt-Flecken") wird mit dieser Reform hinfaellig;
muss in der Umsetzung angeschaut werden. `tests/test_culling.cpp:4428` und
`tests/test_material_graph.cpp:2762` pruefen nur Namen/Praesenz, nicht die Blend-Logik, bleiben
unberuehrt.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile 360-384),
`src/HE_Rendering/src/GiAutoLandscape.cpp:39` (Konsument, pruefen), `tests/test_landscape_groups.cpp`,
`tests/test_culling.cpp`, `tests/test_material_graph.cpp` (nur Namens-/Default-Checks, sollten
gruen bleiben).

## 4. Vorschlaege fuer den Fels-Uebergang

Alle drei ersetzen Zeile 428-430 (`rockT`/`r.rockMask`) und verbreitern zentriert auf den ALTEN
Mittelpunkt (nicht nur vorwaerts von `pRockSlope` aus, sonst verschiebt sich die ganze Rampe und
"sehr steil" waere am oberen Rand der Rampe (Steigung ~0.531, ~62°, `HE_DUMP_AUTOLANDRAMP=32`)
nicht mehr reiner Fels): `rockE0' = pRockSlope − 0.5×pRockBlend`, `width' = 2×pRockBlend`. Bei
Mountains-Defaults (`pRockSlope=0.12`≈28°, `pRockBlend=0.12`): alt `[0.12, 0.24]` (28°-41°), neu
`[0.06, 0.30]` (20°-46°) — der Uebergang beginnt frueher und endet spaeter, doppelt so breit,
zentriert auf denselben Mittelpunkt (34° statt am alten Rand). Die Rock-by-Size-Baender
(`tMid`/`tBig`) wuerden in der echten Umsetzung dieselbe Verbreiterung bekommen (zentriert auf
`midSlope`=0.26≈42° bzw. `pBoulderSlope`=0.40≈53°) — fuer den Masken-Render hier nicht noetig,
da `r.rockMask` davon unabhaengig ist (siehe §2).

### V-A — Breiter Rauschversatz (ein grossskaliges, nullmittelwertiges Fbm)

**Beschreibung.** Ein einzelnes Fbm (14 m Wellenlaenge, Weltraum-XZ, derselbe Knotentyp wie das
entfernte `dirtFbm`, ganzzahliger Hash wie ueberall in dieser Datei) wird nullmittelwertig gemacht
(`fbm − 0.5`) und mit `pRockBlend` skaliert zu `r.slope` addiert, BEVOR die verbreiterte Rampe
greift: `rockT = r.slope + breakup + heightBias(...)`, `r.rockMask = smoothstep(rockE0', width',
rockT)`. Weil `xz` konstant entlang Z variiert und die Terrain-Geometrie selbst nicht, ist jede
Z-Abhaengigkeit im Bild einzig dieser Term — die Kante folgt keiner einzigen Iso-Steigungs-Linie
mehr. Mischreihenfolge: (Gras→Dirt aus §3) → Fels, unveraendert in der Reihenfolge, nur die
Fels-Schwelle selbst wird breiter und ortsabhaengig.
**Bild.** `slope-before-masks.jpg` / `v-a-rock-after-masks.jpg`. Sichtbarer Unterschied bei
diesem Kamerawinkel/Zoom moderat (siehe Pixel-Diff unten) — die Verbreiterung macht sich vor
allem als ein paar Bildzeilen mehr Streuband bemerkbar, nicht als dramatischer Look-Wechsel; eine
staerkere Amplitude (z. B. `pRockBlend×1.5` statt `×1`) waere ein erster Tuning-Hebel.
**Kosten.** +1 `Fbm` (~0.1×der Kosten von 3 bereits bezahlten gebombten Texturleseoperationen) +
~6 ALU, NUR dort ausgewertet, wo `rockCan` gilt (Zeile 388) — auf einem typischen Terrain ein
kleinerer Anteil der Flaeche als die Flaeche, auf der frueher `dirtFbm` lief. Nicht gemessen
(Framezeit).
**Risiko.** Niedrig an Kosten, mittel an Amplitude/Wellenlaenge-Tuning: ein zu kleiner Wert zeigt
kaum Wirkung (siehe Bild), ein zu grosser lässt Fels "Inseln" weit ausserhalb der eigentlichen
Steigung erscheinen (das Fbm kann `r.slope` lokal ueber `pRockSlope` heben, auch auf nahezu
flacher Flaeche, wenn `breakup`s Amplitude relativ zu `rockE0'` zu gross gewaehlt ist — muss gegen
echte Rock-Fotos, nicht nur Platzhalter, gegengeprueft werden).
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile 428-430, plus
dieselbe Formel fuer `tMid`/`tBig`, Zeile 404-405, in der echten Umsetzung).

### V-B — Domain-Warp (zwei Fbm fuer einen Versatz, drittes Fbm an der verschobenen Position)

**Beschreibung.** Wie V-A, aber statt den Wert EINES Fbm direkt auf die Schwelle zu addieren, wird
zuerst eine 2D-Versatzkarte aus ZWEI dekorrelierten, grosswelligen Fbm-Lesungen gebaut (je ein
eigener `ConstVec2`-Offset, 20 m Wellenlaenge), die `xz` um bis zu 6 m seitlich verschiebt; ein
DRITTES Fbm wird an dieser VERSCHOBENEN Position gelesen und genauso wie in V-A auf `rockT`
addiert (IQ-Domain-Warp). Der Unterschied zu V-A: die Bruchlinie folgt keiner einzigen
Vorzugsrichtung mehr (ein einzelnes Wertrauschen, auf eine 1D-Schwelle addiert, kann bei wenigen
Oktaven noch als eine Art verwaschene Sinuswelle lesen) — Mischreihenfolge identisch zu V-A.
**Bild.** `slope-before-masks.jpg` / `v-b-rock-after-masks.jpg`. Bei diesem Kamerawinkel/Zoom kaum
von V-A zu unterscheiden (die Domain-Warp-Eigenschaft — keine Vorzugsrichtung — zeigt sich erst
auf einer groesseren, weniger gleichfoermigen Hangflaeche als dieser 32 m-Rampe; fuer einen
aussagekraeftigeren A/B braucht es die echte Mountains-Geometrie aus Schritt 5/6, nicht die
analytische Witness-Rampe).
**Kosten.** 3 `Fbm`-Auswertungen statt V-As 1 (3x der Rauschkosten, immer noch nur unter
`rockCan`), + 2 `ConstVec2`, + ~10 ALU fuer den Versatz. Nicht gemessen.
**Risiko.** Hoeher an Graph-Komplexitaet (9 neue Knoten statt V-As 5) und Kosten als V-A fuer einen
Qualitaetsgewinn, der sich in diesem Bildpaar nicht zeigt; ob der Mehraufwand den Unterschied
wert ist, kann erst auf der echten Mountains-Geometrie beurteilt werden. Zwei der drei Fbm-Seeds
(`37.1/91.3`, `211.7/58.4`) sind willkuerlich gewaehlt Platzhalter, muessten von den uebrigen
Offsets der Datei (`basin`s `173.1/419.7`) dekorreliert bleiben.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile 428-430 + 2 neue
`ConstVec2`, plus dieselbe Formel fuer `tMid`/`tBig` in der echten Umsetzung).

### V-C — Breiter Blend + Texturrelief-Verstaerkung (keine neue Rauschquelle)

**Beschreibung.** Kein neuer Knoten: nur die Rampe verbreitern (`rockE0'`, `width'` wie oben) UND
den bestehenden `heightBias(rock.height, groundSplit, …)`-Aufruf (Zeile 429, liest ohnehin schon
gelesene Hoehenkanaele von Fels und Boden) mit der NEUEN breiten Breite statt der alten
`pRockBlend` skalieren. Das bereits vorhandene photografische Hoehenrelief der gebombten
Fels-Textur (heute mit kleiner Skala kaum sichtbar) bekommt dadurch mehr Einfluss auf die
Schwelle — die Aufbrechung kommt aus echtem Texturdetail, nicht aus zusaetzlichem Prozedural-Rauschen.
Mischreihenfolge identisch zu V-A/V-B.
**Bild.** `slope-before-masks.jpg` / `v-c-rock-after-masks.jpg` — im Vergleich zu V-A sichtbar
aehnlich (das bestehende Relief liefert bereits die feine Streuung, die in der Baseline schon
leicht zu sehen ist, siehe §1), ohne dass ein einziger neuer Knoten hinzukommt.
**Kosten.** Null neue Knoten, Null neue Texturzugriffe — nur zwei Konstanten (`×2.0`, `×0.5`) in
bereits vorhandenen Multiply-Aufrufen. Die guenstigste der drei Varianten.
**Risiko.** Am hoechsten abhaengig von den ECHTEN (noch nicht gepackten, Schritt 6 wartet auf die
Texturauswahl) Fels-Fotos: ein kontrastarmer Hoehenkanal liefert kaum Aufbrechung, das Ergebnis
waere dann nur "breiter", nicht "aufgebrochen" — im Gegensatz zu V-A/V-B ist die Wirkung hier
NICHT durch einen eigenen Amplitude-Parameter garantiert, sondern reiner Nebeneffekt der
Textur. Muss gegen die reale Textur aus Schritt 6 geprueft werden, sobald die vorliegt.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile 428-430, plus
dieselbe Verbreiterung — ohne eigene Breakup-Quelle, da dort kein zweites Hoehenpaar existiert —
fuer `tMid`/`tBig`, Zeile 404-405, in der echten Umsetzung).

## 5. Vorschlag zur Blend-Reihenfolge der Rock-Groessen (strukturell, NICHT gerendert)

### V-D — Durchgehende Dreier-Ueberblendung statt gestapelter Baender

**Beschreibung.** Heute blendet Mountains' Rock-by-Size klein→mittel→gross als ZWEI
nacheinander geschaltete, je `pRockBlend`=0.12 breite `blend()`-Aufrufe (Zeile 404-422): "mittel"
startet exakt bei `midSlope`=0.26 (~42°) und ist exakt bei +0.12 (~1 Rampenbreite) fertig,
"gross" exakt bei `pBoulderSlope`=0.40 (~53°) — selbst nach V-A/B/Cs Breakup auf der AEUSSEREN
Gras/Dirt-vs-Fels-Kante bleiben das zwei eigene, schmale Baender, weil sie eine eigene,
unveraenderte Rampenbreite benutzen. V-D ersetzt die zwei sequenziellen Blends durch EINE
Drei-Wege-Mischung aus dreieckigen Gewichten, deren Breite nicht mehr `pRockBlend` ist, sondern
der volle Abstand zum jeweiligen Nachbar-Anker (klein↔mittel: `midSlope − pRockSlope` ≈ 14°,
mittel↔gross: `pBoulderSlope − midSlope` ≈ 11°, statt vorher 0.12 ≈ 8° fuer beide) — auf `pRockSlope`,
`midSlope`, `pBoulderSlope` zentriert und auf Summe 1 normalisiert (`wSmall + wMedium + wLarge = 1`),
dann `rock = wSmall·rockSmall + wMedium·rockMedium + wLarge·rockLarge` je Kanal. Mischreihenfolge:
die drei Rock-Groessen ueberlappen jetzt durchgehend ueber die GESAMTE Fels-Zone, statt in drei
disjunkten Baendern mit scharfen Uebergaengen aneinanderzustossen; kombiniert mit V-A/B/Cs Breakup
auf der aeusseren Kante fuer die vollstaendige Behebung beider in Runde 1 gemeldeter Symptome
(harte Fels-Kante UND zusaetzliche Groessen-Baender).
**Bild.** NICHT gerendert — der `masks`-Witness-Modus zeigt nur `r.rockMask` (Fels vs. Boden),
nicht welche der drei Rock-GROESSEN an einer Stelle gezeichnet wird; dieser Unterschied ist nur in
texturiertem/lit-Material sichtbar (unterschiedliche Foto-Kacheln je Groesse). Ein texturierter
Vorher/Nachher-Vergleich braucht die `landscape_tex_gen --material`-Regenerierung von
`M_AutoLandscape.hasset`, die dieser Schritt bewusst nicht anfasst (Asset-Datei, auch wenn
temporaer erlaubt — der Mehraufwand stand in keinem Verhaeltnis zum Nutzen fuer einen rein
strukturellen Vorschlag, der ohnehin erst mit den echten Fels-Fotos aus Schritt 6 sinnvoll zu
beurteilen ist).
**Kosten.** Ersetzt 2 sequenzielle `Lerp`-Blends + 2 schmale `If`-Gates (mit `eps`=0.01-Rand) durch
3 Dreiecks-Gewichte (~3x Smoothstep-aehnliche ALU) + Normalisierung (2 Add, 1 Divide, 2 Multiply) +
weiterhin 2 `Lerp`. Netto grob 2x die ALU der heutigen Groessen-Blendstufe, keine neuen
Texturzugriffe, kein neues Rauschen (Breakup bleibt V-A/B/Cs eine gemeinsame Quelle fuer die
aeussere Kante). Nicht gemessen.
**Risiko.** Mittel: die heutigen `smallCan`/`mediumCan`/`largeCan`-Gates (Zeile 406-417) sparen die
9 gebombten Texturzugriffe einer Groesse, wo sie NICHT gezeichnet wird (Kommentar Zeile 332-334);
mit durchgehender Ueberlappung ueber die GESAMTE Fels-Zone bleiben alle drei Groessen ueber einen
groesseren Flaechenanteil aktiv als heute — mehr Textur-Bandbreite im Schnitt, der Betrag ist nicht
gemessen. Ausserdem: alte Szenen mit einem von Hand gesetzten `Boulder Slope` nah an `Rock Slope`
(sehr schmaler mittlerer Bereich) wuerden mit "volle Nachbarabstand-Breite" ein sehr breites,
moeglicherweise unbeabsichtigtes Boulder-Band bekommen — die Umsetzung braucht eine Unter-/Obergrenze
fuer die Dreiecksbreite, nicht einfach den vollen Anker-Abstand.
**Dateien.** `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp` (Zeile 400-422, die
komplette Rock-by-Size-Blendstufe).

## 6. Pixel-Beleg (sampled, jeder 2. Pixel, |ΔRGB| > 10 als "unterschiedlich" gezaehlt)

`slope-before-masks.jpg` gegen `v-a-rock-after-masks.jpg`: 8827 von ~115200 sample-Punkten
unterschiedlich (max. Kanal-Delta 158) — der Shader hat real reagiert; der Effekt ist bei diesem
Kamerawinkel/Zoom in Bruchteilen uebereinstimmend mit der Beschreibung (etwas breiter, etwas mehr
Streuung an der Kante), aber visuell weniger dramatisch als die verworfenen Mockups aus Runde 1.
Das ist ehrlich so zu nehmen: eine Amplitude/Breite, die bei `pRockBlend=0.12` kaum auffaellt,
kann in der Umsetzung hochgedreht werden, sobald sie gegen die echten Fels-Fotos (Schritt 6)
getestet wird — eine Entscheidung, die dieser Vorschlags-Schritt bewusst offen laesst.

## 7. Randbedingungen, eingehalten

- **R-1 (Textur-Cap 4, vorhandene Slots wiederverwenden):** keiner der vier Vorschlaege liest
  eine neue Textur oder fuegt einen neuen `MatNodeType` hinzu. V-A/V-B/V-C nutzen ausschliesslich
  bereits im Graphen bzw. in dieser Datei verwendete Knotentypen (`Fbm`, `ConstVec2`, `Add`,
  `Subtract`, `Multiply`, `Smoothstep` ueber `w.ramp`); V-D nutzt nur bereits gelesene
  Texturen (`rockSmall`/`rockMedium`/`rockLarge`, unveraendert) in neuer Gewichtung.
- **Alte Szenen laden unveraendert:** reine Aenderungen an `buildAutoLandscapeGraph`, beim
  naechsten Laden aus dem Code neu erzeugt (`regenerateMaterialFromGraph`), kein Format-, Chunk-
  oder Parameter-Slot-Wechsel. `DirtAmount`/`DirtSize` bleiben als Parameter bestehen (§3), nur
  ohne Wirkung auf die Dirt-Platzierung.
- **Nicht in diesem Schritt:** keine Umsetzung im Repo (alle vier Patches wurden nur lokal zum
  Rendern angewendet und zurueckgesetzt), keine neuen Layer/Texturen, kein RVT-Umbau, V1/V2 aus
  Runde 1 nicht neu bewertet. `git diff` gegen den Remote-Branch zeigt nur `docs/`.

## 8. Naechster Schritt (auf Nachfrage)

Der Mensch waehlt einen oder mehrere der vier Vorschlaege (V-A/V-B/V-C fuer die aeussere Kante
sind untereinander austauschbar/kombinierbar, V-D fuer die Rock-Groessen-Baender ist unabhaengig
und kann mit jedem der drei kombiniert werden); ein eigener Schritt setzt genau das um, mit
echtem Vorher/Nachher im LIT-Modus (nicht nur der Maske) auf der echten Mountains-Geometrie aus
Schritt 5/6, Metal/GL-Vergleich, und — falls V-C gewaehlt wird — einem Blick auf die echten
(nicht nur Platzhalter-)Fels-Texturen, da V-Cs Wirkung von deren Hoehenkontrast abhaengt.

## Handoff

- **Topic / step:** 329, Schritt 11 von (laufend)
- **Branch / commit:** `claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate` @
  `5bc88417d` + dieser Doc-Commit (gepusht: ja — dabei auch Schritt 9s zuvor ungepushter Commit
  `5bc88417d` mitgenommen, der seit seinem Lauf lokal auf `bb855163f` sass)
- **Done:** 4 Vorschlaege (V-A/V-B/V-C fuer den Fels-Uebergang, V-D fuer die Rock-Groessen-Baender)
  mit Formel, Mischreihenfolge, Kosten, Risiko, Dateien; echte Engine-Renderings fuer V-A/B/C
  (masks+ground-Witness-Modus), V-D explizit "nicht gerendert" mit Begruendung. Keine Aenderung
  im Repo ausser `docs/`.
- **Files touched:** nur `docs/landscape-slope-scatter-proposals-round2-2026-11-18.md` + 6 Bilder
  in `docs/img-landscape-slope-scatter-proposals-round2-2026-11-18/`. Temporaer (nicht committet)
  bearbeitet und wieder zurueckgesetzt: `src/HE_Core/src/MaterialGraph/AutoLandscapeMaterial.cpp`
  Zeile 359-384 (Dirt) und 428-430 (Fels-Schwelle), je drei Mal (V-A, V-B, V-C).
- **Verified:** `git status`/`git diff` nach jedem Build-Zyklus und am Ende leer; Release-Build
  dieses Branches (`C:/hwLS11`, x64-release, `-DDEPLOY_DIR`/`-DHE_ENGINE_SDK_DIR` Overrides)
  erfolgreich fuer Baseline + V-A + V-B + V-C; Pixel-Diff Baseline gegen V-A (8827/~57600
  Sample-Punkte, siehe §6) zeigt, dass der Code-Pfad real griff, nicht nur unveraendert blieb.
- **Not verified:** keine Tests gelaufen (reiner Vorschlags-/Render-Schritt, kein Code im Repo
  geaendert, der eine Testpflicht ausloest); D3D/Vulkan/Metal nicht angefasst (nur GL, wie Runde
  1); kein lit/texturierter Vergleich (nur die Masken-Witness-Modi, siehe §2 und §5 fuer die
  Begruendung bei V-D).
- **Decisions taken:** Masken-Witness (`=masks`/`=ground`) statt des Default-lit-Modus fuer
  echte, direkte, Asset-Bake-unabhaengige Renderings — vermeidet den `landscape_tex_gen
  --material`-Umweg (die gebackene `M_AutoLandscape.hasset` wird NICHT beim Laden aus dem Code
  neu erzeugt, nur beim Speichern/Regenerieren ueber den Generator; der lit-Default-Modus haette
  also die alte, ungepatchte Shader-Fassung gezeigt). Dirt-Rampe faellt NICHT vor der
  Fels-Schwelle auf Gras zurueck (§3) — ein Rueckfall waere ein dritter, neuer Rand gewesen.
  Verbreiterung zentriert auf den alten Mittelpunkt, nicht nur vorwaerts von der alten Schwelle
  (§4), damit "sehr steil" am oberen Rampenrand reiner Fels bleibt.
- **Decisions suggested, not taken:** welcher von V-A/V-B/V-C (Noise-Staerke, Kosten, Risiko
  gegeneinander abwaegen); ob V-D unabhaengig oder erst nach den echten Fels-Fotos aus Schritt 6
  umgesetzt wird.
- **Pending:** Schritt 6 wartet weiter auf die menschliche Textur-Zuordnung (offene Anfrage #183);
  die Umsetzung des gewaehlten Vorschlags ist ein eigener Schritt.
- **Traps:** `half` (ConstFloat 0.5) wird weiter unten (Zeile ~624, Debug-Normal-Ansicht Spalte 8)
  wiederverwendet — wer den Dirt-Block komplett ersetzt, muss die Konstante dort neu deklarieren
  oder der Build bricht mit C2065 ab (passiert, gefixt). `masks`/`ground`-Witness-Modus zeigt NICHT
  die Rock-by-Size-Textur-Wahl, nur `r.rockMask`/`r.dirtMask` — fuer V-D-Evidenz braucht es den
  lit-Modus + Asset-Regenerierung.
- **Next steps:** 1. Mensch waehlt V-A/V-B/V-C (+ optional V-D). 2. Eigener Schritt implementiert
  im Repo, mit lit/Metal/GL-Vergleich auf der echten Mountains-Geometrie. 3. Falls V-C: gegen die
  echten Fels-Fotos aus Schritt 6 pruefen, sobald die Textur-Zuordnung vorliegt.
