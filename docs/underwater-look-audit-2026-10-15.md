# Unterwasser-Look: Bestandsaufnahme (Thema 205, Schritt 1)

Stand 15.10.2026, Zweig `claude/unterwasser-postfx-wirkt-billig-caustics-wasseroberfl-chen-u` auf `release/0.7.0`
(934bb7bd). Gegenstand ist die Unterwasser-PostFX aus Thema 185 (Nebel + Kaustik, PR #140) und die Wasserflaeche
von unten. Kein Engine-Code geaendert.

## Vorgabe des Menschen

> das problem ist der unterwasser postfx sieht recht billig aus. caustics sollten sichtbarer am rand sein da wo das
> gewaesser flacher ist und langsam verschwinden bis zu einer bestimmten tiefe und die unterseite der
> wasseroberflaeche sollte auch deutlich geiler mit caustics und durchscheinendem licht und lichtbrechung die sollte
> durchsichtig sein wenn man nah dran ist ansonsten sollte dort auch der blaue nebel sein

Seine zwei Screenshots lagen nur im Chat; nachgestellt ist nach der Beschreibung der Queen (Post 1387):
(1) Blick nach oben auf die Flaeche: diffuse helle Flecken auf trueber blauer Flaeche, keine Brechung, keine
Transparenz. (2) Blick auf den Boden (Gras): grosse, weiche, undefinierte helle Ovale ohne Tiefenbezug.

## Nachstellung

`scripts/underwater_look_shots.py` (neu) auf dem Witness `HE_DUMP_WATERLAKE=bowl` aus Thema 185: Flaeche bei Welt-Y
302, Boden `0,004 r^2 - 6` unter der Landschaft bei Y 300 (Mitte 8 m tief, Ufer bei r = 44,7 m), TOD 0,45,
AA/Bloom aus, 1280x720, Standardwerte des Koerpers. Jede Ansicht zusaetzlich mit `UWFOG=0`.

    python3 scripts/underwater_look_shots.py --editor out/deploy/Editor/HorizonEditor --out /tmp/uwlook --rhi Metal

| Bild | Kamera | entspricht |
|---|---|---|
| `docs/images/underwater-look-up-metal.png` | 2 m tief, Blick 35 Grad nach oben | Screenshot 1 |
| `docs/images/underwater-look-up_nofog-metal.png` | dasselbe ohne Nebel | was unter dem Nebel liegt |
| `docs/images/underwater-accept-metal-defaults.png` (Thema 185, byte-gleich nachgestellt) | 2 m tief, Boden 4,9 m, 20 Grad nach unten | Screenshot 2 (Boden hier untexturiert grau statt Gras) |
| `docs/images/underwater-look-shore-metal.png`, `..._nofog` | 1 m tief, Richtung Ufer, Boden 2,8 m bis 0 m | "am Rand, wo es flacher ist" |
| `docs/images/underwater-look-dry_shallow-metal.png` | 3 m ueber der Flaeche, 40 Grad hinab ins Flache | Beobachtung, siehe 5 |

Die Boden-Ansicht `floor` ist byte-gleich (PNG-md5 da938e78) zum Abnahmebild `underwater-accept-metal-defaults.png` aus Thema 185 (gleiche Kamera,
gleicher Stand), die Nachstellung trifft also den ausgelieferten Effekt. Beide Screenshots des Menschen sind
wiedererkennbar nachgestellt: oben eine truebe graugruene Platte mit hellen Flecken, unten weiche Ovale.

Nur Metal gerendert. Der GL-Shader ist derselbe Text (OpenGLRenderer.cpp `kUnderwaterCausticsFS`), Thema 185 hat Metal
und GL auf etwa ein Promille gleich gemessen; D3D/Vulkan tragen dieselbe Formel (HlslSources.h,
`postfx_underwatercaustics.frag`).

## Wie es heute gebaut ist

- **Kaustik** (`src/HE_Rendering/shaders/postfx_underwatercaustics.frag`, GL-Zwilling OpenGLRenderer.cpp:5488-5543):
  Vollbild-Pass nach der Szene. Aus der Tiefe wird der Weltpunkt P, der gebrochene Sonnenstrahl trifft die flache
  Ebene in S, dort wird die Kruemmung der drei Wellenzuege des Wassermaterials (`kWaveA/B/C`, Wellenlaengen 14 / 5 /
  1,6 m) plus je einem Begleiter (57 Grad gedreht, 0,73x Laenge) summiert, `det` der Abbildung
  Oberflaeche -> Boden gibt die Buendelung. Licht nur wo `1/|det| > 2`, weich abgestuft mit `x/(x+2)`,
  mal `exp(-Tiefe/14 m)`, mal `smoothstep(0, 0,4 m, Tiefe)`, rein additiv.
- **Nebel** (`postfx_underwaterfog.frag`, im GL-Pass gefaltet): Beer-Lambert ueber die nasse Strecke des Sichtstrahls,
  `1 - exp(-0,45/m * Strecke)`, eine Farbe pro Frame.
- **Steuerung** (`RenderExtractor.cpp:1237-1256`, `WaterField.h:160-232`): Nebelfarbe = Mischung shallow/deep nach
  der Tiefe der KAMERA; Kaustik-Intensitaet 0,55 x Sonnenfarbe, ueber die ersten 0,5 m Kameratiefe eingeblendet
  (`causticsFadeIn`), und nur bei nasser Kamera (`if (!out.wet) return`).
- **Wasserflaeche**: das Graph-Material `Water.hasset` (MatGen `main.cpp`), transluzent, alle Draws zweiseitig
  (MatGen-Kommentar Zeile 865: `doubleSided` "Not read by any backend today"). Die Flaeche steht nicht im
  Tiefenpuffer, den die Post-Passes lesen (Abnahme-Doku 185, Abschnitt 8.3).

## Ursachen

### 1. Blob-Form

Die Kaustik ist ein einziger, weicher, analytischer Term aus wenigen langen Wellen. Es gibt keine Kaustik-Textur und
keine Schaerfung.

- Die Zellgroesse kommt aus den Wellenlaengen 1,6 bis 14 m. Echte Kaustik-Netze entstehen aus der Feinkraeuselung
  im Zentimeter- bis Dezimeterbereich; die fehlt im Modell ganz (nur als konstanter Zuschlag `+2` in `kd`).
- Die Kruemmung beherrscht der 1,6-m-Zug (Kruemmung k * Steilheit = 0,35, der 14-m-Zug 0,03) und sein Begleiter
  (0,29). Zwei nahezu sinusfoermige Gitter, mit der Schwelle `1/|det| > 2` geschnitten, ergeben einzelne
  Linsen statt eines zusammenhaengenden Netzes aus duennen Linien.
- `x/(x+2)` und die Kappe `max(|det|, 0,1)` machen jeden Brennpunkt zu einem weichen Plateau statt einer Linie;
  die Phasenverbiegung `S += 1,6 * sin(...)` verschmiert sie weiter.
- Auf dem Boden unter flachem Blickwinkel zieht die Perspektive die Linsen zu den Ovalen, die der Mensch beschreibt.
- Rein additiv: zwischen den Linien wird nichts dunkler, der Kontrast bleibt flach. Echte Kaustik verteilt Licht um.

### 2. Fehlende (bzw. verkehrte) Tiefenabhaengigkeit

Das Netz ist im Flachen aus und im Tiefen am staerksten, genau umgekehrt zur Vorgabe.
`scripts/underwater_caustics_coverage.py` rechnet die Shaderformel bei Standardwerten auf 40000 Oberflaechenpunkten nach:

| Bodentiefe | Anteil mit Netz | mittleres Netz nach Tiefenfade |
|---|---|---|
| 0,1 / 0,25 / 0,5 / 1 m | **0 %** | 0 |
| 2 m | 1,0 % | 0,0005 |
| 3 m | 4,1 % | 0,0034 |
| 5 m | 12,1 % | 0,018 |
| 8 m | 22,6 % | 0,048 |
| 14 m | 38,5 % | **0,081 (Maximum)** |
| 25 m | 39,1 % | 0,034 |

Mechanismus: `kd = (eta - 1)/eta * (0,6 * Tiefe + 2)` waechst mit der Tiefe, und die Schwelle `1/|det| - 2 > 0`
wird erst bei grossem `kd` ueberschritten. Der Fade `exp(-Tiefe/14 m)` ist viel zu langsam, um das umzudrehen, und
`smoothstep(0, 0,4 m)` schaltet die letzten 40 cm am Ufer zusaetzlich ab.

Im Bild bestaetigt (Metal, Netz an gegen `UWCAUSTICS=0`, ohne Nebel): Ufer-Ansicht, Boden 0 bis 2,8 m:
**0,14 %** der Pixel aendern sich (mittlere Abweichung 0,007 von 255). Boden-Ansicht, Boden 4,9 m: **5,7 %**
(0,41), wie in Thema 185 gemessen (5,74 %).

Fuer Schritt 2 wichtig: `causticsFadeIn` haengt an der Tiefe der Kamera, nicht des Bodens, und bei trockener Kamera
gibt es gar kein Netz. Vom Ufer oder Boot aus sieht man im Flachen also nie Kaustik.

### 3. Fehlende Unterseiten-Darstellung

Beweis: die Aufnahme nach oben ist mit Kaustik an und aus **byte-gleich**, mit und ohne Nebel
(md5 `e5ad1e34...` ohne Nebel, `f9be117e...` mit Nebel, je an = aus). Kontrolle: derselbe Schalter aendert den Boden
(5,7 %, siehe oben). Die Flecken, die der Mensch an der Unterseite als Kaustik liest, sind keine: es ist das
Oberseiten-Material (Himmelsspiegelung, Schaumspitzen) mit nach oben zeigender Normale, zweiseitig gezeichnet
(`underwater-look-up_nofog-metal.png`: graue Platte mit beige-blauen Flecken).

Strukturell: Die transluzente Flaeche steht nicht im Tiefenpuffer, die Kaustik ueberspringt Himmelspixel
(`d >= 1`). **Kein Post-Pass kann die Unterseite bemalen.** Kaustik, durchscheinendes Licht und Brechung von unten
muessen ins Wassermaterial (Zweig "von unten gesehen", Normale gedreht) oder in einen eigenen Unterseiten-Draw.
Es fehlen dort: Snell-Fenster (Himmel nur innerhalb ca. 48,6 Grad um die Senkrechte), Totalreflexion ausserhalb
(Spiegelung von Boden/Tiefenfarbe), durchscheinendes Sonnenlicht, bewegte Lichtmuster.

"Nah dran durchsichtig" ist mit den Standardwerten auch mit perfekter Unterseite unmoeglich: bei 0,45/m ist die
Flaeche in 1 m Abstand schon zu 36 % vernebelt, in 2 m zu 59 %, in 3,5 m (die `up`-Ansicht) zu 79 %.

### 4. Nebel

Eine Farbe pro Frame (Funktion der Kameratiefe), Menge nur nach Weglaenge. Kein Verlauf nach oben heller / nach unten
dunkler, kein Aufhellen Richtung Sonne (Vorwaertsstreuung), keine wellenlaengenabhaengige Absorption (Rot zuerst).
Jede Blickrichtung wird dieselbe flache Waschung (`underwater-look-up` gegen `underwater-accept-metal-defaults`: oben
grau-gruen, unten blaugruen, beide strukturlos). Lichtschaechte gibt es nicht (Thema 185: nie gebaut).

### 5. Nebenbefund (nicht Ursache)

Von oben ins Flache (`underwater-look-dry_shallow-metal.png`) ist der Boden kaum zu sehen und es gibt kein Netz
(Kamera trocken). Das Wassermaterial von oben ist nicht Thema 205; erwaehnt, weil "Kaustik am Rand" auch von oben
erwartet werden duerfte.

## Folgerungen fuer Schritt 2 und 3

- Schritt 2 (Kaustik): Netz aus feiner, breitbandiger Struktur (Kaustik-Textur oder prozedural mit vielen
  Richtungen/Skalen), scharfe Linien, Energie umverteilen statt nur addieren; Staerke nach BODEN-Tiefe, im Flachen am
  staerksten, weicher Fade bis zu einer einstellbaren Tiefe; auch bei trockener Kamera (Ufer). Der `smoothstep(0, 0,4)`
  am Ufer und `kd ~ Tiefe` muessen weg. Die Parameter `scale/depth` aendern damit ihre Bedeutung (Tests in
  `test_underwater_camera.cpp` beachten).
- Schritt 3 (Unterseite): im Material bzw. eigenen Draw, nicht im Post-Pass. Nebel auf der Unterseite dort selbst
  rechnen (Abstand Kamera -> Flaechenpunkt), sonst wird die Flaeche doppelt oder gar nicht vernebelt. Standard-Dichte
  0,45/m ueberdenken.
- Abnahme: fuer jede Aenderung A/B mit `scripts/underwater_look_shots.py` gegen die Bilder hier; die
  Unterseiten-Aufnahme ist der Zeuge, der heute byte-gleich bleibt und nachher nicht mehr darf.
