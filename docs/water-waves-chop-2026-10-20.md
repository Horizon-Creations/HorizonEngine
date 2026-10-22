# Wasser: feinerer Chop (Thema 206, Schritt 3)

Stand 20.10.2026, Mac M5. Nur `src/HE_Tools/src/MatGen/main.cpp` (Wassergraph) und das neu erzeugte
`Water.hasset`; keine Physik-Konstanten, keine Parameter, keine neuen Knotentypen.

## Handoff

### Befund (Baseline gegen Referenz 29)
`baseline-metal-open(-hi).png` / `after-*` (Schritt 2, Wellen unverändert) gegen Ref 29: Ref zeigt kurze,
unregelmäßige Wellchen **auf jeder Skala von einem Meter bis zur Handbreite**, nur an der Flanke hell, dazwischen
ruhige Flecken. Unser Wasser: grobe Dünung plus zwei Chop-Lagen (2,6 m / 0,9 m, Neigung ~3 Grad). Das Nahfeld
blieb eine glatte dunkle Fläche mit vereinzelten Glanzpunkten; die Lücke zwischen 0,9 m und der Pixelgröße war leer
und die Streifen kamen fast nur von den Sinuszügen (zu regelmäßig).

### Geändert (`MatGen/main.cpp`)
- Chop-Abschnitt (~Zeile 755-790): vier statt zwei `rippleLayer`: 2,6 / 0,9 / **0,4 / 0,17 m** je Rauschzelle.
  Stärken relativ zu `drive` (Summe der Steilheiten): `kChopBase` 0,2 → **0,3**, `kChopMid` 0,55 → **0,9**,
  neu `kChopFine` **0,8**, `kChopFiner` **0,55** (Konstanten vor `rippleLayer`, ~Zeile 449).
- Eigene Entfernungsblende für die feinen Lagen (`fadeF` 7..40 m, `fadeG` 3..14 m, ~Zeile 712); die groben
  (`fadeR` 14..70 m) bleiben. Grund: 0,17 m-Zelle hat Oktaven von 2 cm, unter einem Pixel ab ~14 m bei 25 Grad.
- Neue Lagen driften gegenläufig (`b` -0,8x, `c` -1,3x) und mit anderer Richtung als die alten, damit das Bild nicht
  als Ganzes kriecht. Alle vier Lagen laufen durch dieselbe Summe wie vorher: Gust-Faktor, danach Ringneigung, ein
  `Normalize3`. Neue Knoten: nur Multiply/Add/Subtract/Smoothstep, kein Divide/Power/Sqrt, also keine NaN-Gefahr im
  If-Zweig. Kosten: +6 FBM-Lesungen je Pixel (vorher 6 für Chop, jetzt 12), die feinen nur im Nahfeld nötig, der
  Graph rechnet sie aber überall (ein Zweig lässt sich nicht überspringen); GPU-Zeit **nicht gemessen**.
- `scripts/water_baseline_shots.py`: neue Ansicht `far` (400 m Ebene, 3 m hoch, 6 Grad Neigung) für Horizont/Fades.
- `EditorDeps/EngineContent/Materials/Water.hasset` neu (mat_gen zweimal, `cmp` gleich) und nach
  `out/deploy/Editor/EngineContent/Materials` kopiert.

### Gemessen
Bilder `docs/water-realistic-baseline/waves-<metal|opengl>-<open|open-hi|far|shore|beach>.png`; `after-*-far.png`
ist der Stand von Schritt 2 für `far`. Zwei Läufe je Ansicht md5-gleich (Metal alle fünf; GL `open/open-hi/far`;
GL `shore/beach` mit Gelände wie in der Baseline wenige Pixel verschieden).
`png_diff.py after-X waves-X` (Schritt 2 gegen jetzt), Metal / GL fast gleich:
`open` 3,05/255 mittel, 63,5 % der Pixel; `open-hi` 0,78, 47 %; `far` 3,25, 51 %; `beach` 0,51, 29 %;
`shore` 0 (Kamera 375 m hoch, Chop schon ausgeblendet). Rauschboden 0,01 %.
Flackern (Aliasing): `far` zweimal mit 3 cm Kameraversatz (Metal), Differenz je Zeilenband, alt → neu:
Horizontband (Zeilen 306-340) 1,28 → 1,37 (praktisch gleich, kein neues Aliasing am Horizont); Mitte (340-420)
2,77 → 5,81; Nahfeld (420-560) 1,64 → 5,97, dabei wächst die horizontale Hochfrequenz-Energie im gleichen Maß
(0,67 → 3,28 im Nahfeld), das Verhältnis Versatz/Inhalt fällt (2,4 → 1,8): mehr Detail, nicht mehr Flimmern je Detail.
Physik: `he_tests --source-file='*test_water*'` 228/228, `*test_engine_materials*` 11/11 (Physik-Konstanten unberührt).

### Entschieden / nur vorgeschlagen
Entschieden: Konstanten im Graph, vier Lagen, getrennte Fades. Durchgespielt und verworfen: Stärken 1,0/0,8 für
Fine/Finer und Fade 3,5..20 (Nahfeld wirkte wie Filmkorn, nicht wie Wellchen). Vorschlag (nicht gebaut): gerichtete,
längs gedehnte oder gewarpte Lagen für „gekräuselte" Kämme statt reinem Wertrauschen (Ref 29 hat Kammlinien); die
Lage ließe sich mit einer zusätzlichen Domänenverzerrung bauen, kostet aber weitere FBM-Lesungen.

### Nicht geprüft
D3D11/D3D12/Vulkan (Schritt 5); Bewegung (Flackern nur über den Kameraversatz-Proxy bewertet, kein Video);
GPU-Zeit der +6 FBM; echte Landschaft mit 1 km Wasser; Sicht von unten (Unterseitenzweig nutzt dieselbe Summe, nicht
angesehen); Ansicht über flachem Grund (Schritt 4). Auffällig, aber nicht Teil dieses Schritts: im Steilblick
(`open-hi`, unteres Drittel) bleibt das Wasser trotz feinerem Normal glatt, weil dort Fresnel klein ist und der
Körperfarbe dominiert; die Chop-Struktur wird dort erst durch Schattierung (Farbe/Transparenz) sichtbar.

### Nächste Schritte
1. Schritt 4 (Transparenz): beim Umbau des Körpers die Detailsichtbarkeit im Steilblick mitdenken (Ref 29 ist von oben).
2. Schritt 5: D3D11/D3D12/Vulkan mit `scripts/water_baseline_shots.py --prefix after` auf NN-WS03; `far`-Ansicht
   gegen Flackern ansehen.
