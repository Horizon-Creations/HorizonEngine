# Rauschtextur-Kosten (B7, Thema 103, Schritt 3), 30.09.2026

Zweig `claude/perf-sky-wolken-optimierungen-a1-a4-b7`, gemessen auf dem Stand nach A4 (`1475eae5`),
Fix in `bac458ab`. Metal, Release, Apple M5.

Ausgangspunkt (Synthese B7): 3D-Rauschtextur 256³ `RG16Unorm` = 64 MiB, ohne Mips,
`StorageModeShared`. Verdacht: der Wolken-Raymarch hängt am Textur-Cache; Vorschlag „`Private` +
Blit-Upload“ (S) bzw. Mips (M). Wirkung „unbekannt, braucht eine GPU-Aufnahme“.

## Ergebnis in drei Sätzen

1. **Erzeugung: 10,7 s CPU beim Renderer-Start**, seriell auf dem Hauptthread, in allen fünf
   Backends, Editor und Spiel. Das hatte das Audit nicht auf dem Schirm. **Behoben, bitgleich:
   0,45 s** (Metal-Renderer-Init 11,6 s → 1,0 s).
2. **Sampling: etwa 1,5 ms des Scene-Passes** (von ~5,5 ms, Landschaft, 2840×1528) hängen am
   Speicher-/Cache-Fußabdruck der Textur. Belegt mit einer Cache-Sonde (32³-Ausschnitt desselben
   Volumens, gleicher Shader, gleiche Fetch-Zahl).
3. **Beide vorgeschlagenen Fixes helfen dagegen nicht:** `Private` 0 bis −0,1 ms (Rauschen),
   Mips **+0,8 bis +1,2 ms** (langsamer). Auf der GPU-Seite ist in diesem Schritt nichts geändert.

## 1. Erzeugung (`HE::BuildSkyNoise3D`)

Gemessen mit einem temporären Zeitstempel um den Aufruf in `MetalRenderer::CreateScenePipeline`
(Zeugenzeilen in `raw-b7/witness-log-lines.txt`):

| Stand | n | Zeit | FNV-1a über die Bytes |
|---|---|---|---|
| vorher (seriell, `hash3` pro Nachbar) | 256 | Median 10 663 ms (9 940 bis 10 762, 27 Läufe) | `0x79c8e92e96950cc9` |
| + Jitter-Tabelle 48³ | 256 | 2 757 ms | `0x79c8e92e96950cc9` |
| + Wrap-Indizes aus der Nachbarschleife | 256 | 2 276 / 2 445 ms | `0x79c8e92e96950cc9` |
| + z-Scheiben auf ≤ 8 Threads (**Commit**) | 256 | 445 / 447 / 450 ms | `0x79c8e92e96950cc9` |

- **Ursache:** Jeder der 16,7 M Voxel hat seine 27 Worley-Nachbarzellen neu gehasht (~453 M
  `hash3` + `glm::mod`), obwohl es nur 48³ = 110 592 Zellen gibt.
- **Warum bitgleich:** Die gewrappte Zellkoordinate ist ein ganzzahliger Float. `hash3` bekommt in
  der Tabelle also exakt die Eingaben, die es inline bekam. Der Abstandsausdruck
  `(off + jitter) - fp` ist unverändert. Jeder Thread schreibt nur seine eigenen Indizes. Belegt
  durch den Digest der Produktionsgröße vor und nach dem Umbau (Tabelle oben).
- **Test:** `tests/test_culling.cpp` „SkyNoise3D: generated volume is byte-pinned“ pinnt jetzt
  zusätzlich n = 256 (die Pins 8/16 laufen seriell, nur 256 nimmt den Thread-Pfad).
  Negativkontrolle: Pin um 1 geändert → Test rot. `he_tests --source-file='*test_culling.cpp,*test_sky_shader.cpp'`:
  56/56 grün.
- **Wirkung beim Start:** `MetalRenderer: initializing` → `initialized` 11,6 s → 1,0 s. Ein
  kurzer Editor-Lauf (10+10 Frames) endet nach 4 s statt 15 bis 17 s. Die anderen Backends rufen
  denselben Builder (`D3D11Renderer.cpp:4802`, `D3D12Renderer.cpp:2344`, `VulkanRenderer.cpp:7108`,
  `OpenGLRenderer.cpp:4016`), dort nicht gemessen.

## 2. Sampling auf der GPU

### Messweg

`he_perf_capture.py --detailed`, Projekt-Kopie von `Test`, `landscape.hescene` (Dome-Wolken coverage
0,5, Terrain), `--cam 0,25,90,0,-0.25`, 2840×1528, 600 Frames nach 300 Warmup, Varianten im Wechsel
mit „shared“ (heute). Maß: exklusive GPU-Zeit des Scene-Passes, p50, als Differenz zum Mittel der
beiden benachbarten „shared“-Läufe. Rohdaten `raw-b7/*.summary.json`.

Gleiches Binary für alle Varianten, umgeschaltet über einen **temporären** Schalter
`HE_SKY_NOISE` an der Texturerzeugung (nicht committet, als Patch in
`raw-b7/b7-probe-switch.patch`). Jede Variante schreibt beim Anlegen eine Zeugenzeile ins Log.

| Variante | was | wozu |
|---|---|---|
| `shared` | heute: 256³ RG16, Shared, keine Mips | Referenz |
| `private` | wie heute, per Blit in eine `Private`-Textur kopiert | der S-Fix aus der Synthese |
| `small` | `BuildSkyNoise3D(32)`, Shader unverändert | erste Cache-Sonde (anderes Rauschen) |
| `crop` | 32³-**Ausschnitt** des echten 256³-Volumens | saubere Cache-Sonde: gleiche Frequenzen und Statistik, gleiche Fetch-Zahl, 128 KiB statt 64 MiB |
| `rg8` | 256³ `RG8Unorm` (Werte gerundet) | halbe Bytes pro Texel bei gleichem Zugriff |
| `mips` | 256³ RG16 mit 9 Mip-Stufen, Sampler `mipFilter linear` | der M-Fix aus der Synthese |

**Bedingungen:** Bildschirm **gesperrt** (`CGSSessionScreenIsLocked`), Stromsparmodus aus, Load 0,5
bis 2,2, GPU vor der ersten Serie 38 % fremd belegt. Absolute Zahlen deshalb nicht mit
entsperrten Messungen vergleichen, nur die Paare.

### Scene-Pass, ms

| Lauf | min | p10 | p50 | Shadow p50 | gpuMs p50 | Hänger | RSS max MB |
|---|---|---|---|---|---|---|---|
| LS0-shared | 4,18 | 5,33 | 5,63 | 0,864 | 9,61 | 2 | 327 |
| LP1-private | 4,17 | 5,25 | 5,62 | 0,867 | 9,70 | 1 | 262 |
| LS1-shared | 2,02 | 5,20 | 5,61 | 0,864 | 9,68 | 2 | 326 |
| LK1-small | 1,15 | 2,69 | 3,98 | 0,850 | 8,60 | 12 | 197 |
| LS2-shared | 4,23 | 5,03 | 5,51 | 0,864 | 9,26 | 1 | 326 |
| LP2-private | 4,07 | 4,92 | 5,40 | 0,865 | 9,08 | 1 | 262 |
| LS3-shared | 4,10 | 5,04 | 5,50 | 0,866 | 9,28 | 1 | 326 |
| LK2-small | 2,61 | 3,44 | 4,16 | 0,849 | 8,93 | 1 | 226 |
| LS4-shared | 4,40 | 5,26 | 6,16 | 0,862 | 10,86 | 2 | 326 |
| LC-S5-shared | 1,76 | 4,27 | 5,72 | 0,866 | 10,22 | 7 | 327 |
| LC-C1-crop | 1,86 | 3,62 | 3,93 | 0,846 | 8,34 | 6 | 262 |
| LC-S6-shared | 4,25 | 5,30 | 5,57 | 0,864 | 9,52 | 1 | 345 |
| LC-C2-crop | 1,28 | 3,79 | 4,06 | 0,850 | 8,49 | 10 | 265 |
| LC-S7-shared | 1,86 | 4,22 | 5,48 | 0,867 | 9,22 | 6 | 326 |
| LR-S8-shared | 4,06 | 4,63 | 5,53 | 0,866 | 9,44 | 0 | 326 |
| LR-R1-rg8 | 3,98 | 4,95 | 5,41 | 0,853 | 9,64 | 0 | 327 |
| LR-S9-shared | 4,17 | 5,26 | 5,67 | 0,866 | 9,81 | 2 | 326 |
| LR-M1-mips | 3,72 | 4,31 | 6,77 | 0,863 | 10,41 | 0 | 326 |
| LR-S10-shared | 3,03 | 3,54 | 5,47 | 0,862 | 9,32 | 0 | 326 |
| LR-R2-rg8 | 3,89 | 4,88 | 5,14 | 0,855 | 9,02 | 1 | 326 |
| LR-S11-shared | 4,46 | 5,34 | 5,64 | 0,868 | 9,66 | 1 | 326 |
| LR-M2-mips | 4,38 | 5,69 | 6,95 | 0,864 | 10,81 | 1 | 327 |
| LR-S12-shared | 2,16 | 4,49 | 6,58 | 0,871 | 11,17 | 5 | 326 |

### Paar-Differenzen (Scene p50, Variante minus Mittel der beiden Nachbarn)

| Variante | Paar 1 | Paar 2 |
|---|---|---|
| `private` | 0,00 | −0,11 |
| `small` | −1,58 | −1,67 |
| `crop` | **−1,72** | **−1,47** |
| `rg8` | −0,19 | −0,42 |
| `mips` | **+1,20** | **+0,84** |

**Rauschboden:** Abstand zweier benachbarter „shared“-Läufe, Median 0,15 ms (0,01 bis 0,94).

### Lesart

- **Der Fußabdruck ist relevant.** Mit dem 32³-Ausschnitt fällt Scene p50 um ~1,5 ms, bei
  gleichem Shader, gleicher Fetch-Zahl und gleicher Rauschstatistik (anders als `small`, dessen
  anderes Rauschen die Wolkenmenge und damit die Licht-Marsch-Fetches ändern könnte; beide Sonden
  landen trotzdem beim selben Wert). Der Wolken-Marsch ist also zu einem guten Teil
  speichergebunden, nicht nur ALU- oder Fetch-Issue-gebunden. Der Verdacht aus S2 2.3 ist damit
  **gestützt**. Ob Cache-Fehlrate oder Speicherbandbreite der Limiter ist, bleibt ohne Zähler offen
  (siehe unten).
- **`Private` bringt keine GPU-Zeit** (0 und −0,11 ms, im Rauschen). RSS fällt um 64 MB
  (326 → 262), weil die Textur nicht mehr in den Prozess abgebildet ist. Der Speicher liegt damit
  weiter im Unified Memory, eine echte Einsparung ist nicht belegt (Footprint nicht gemessen).
- **Mips machen es langsamer** (+0,8 / +1,2 ms). Mit Mip-Filter liest jede trilineare Abfrage zwei
  Ebenen; die hohen Oktaven werden dadurch nicht billig genug, um das auszugleichen. Mips ändern
  außerdem die Optik. Als Hebel verworfen.
- **Halbe Bytes pro Texel (`rg8`)**: −0,19 / −0,42 ms, knapp über dem Rauschboden. RG8 selbst ist
  verlustbehaftet (die 16 bit sollen Bänder an den Schwellrampen vermeiden). Die verlustfreie
  Variante mit demselben Fußabdruck pro Abfrage wären zwei getrennte `R16Unorm`-Volumen (jede
  Abfrage liest nur `.r` **oder** `.g`). Erwartbar also höchstens ~0,3 ms, bei einem Umbau aller
  Rauschfunktionen im Himmels-Shader (neues Texturargument durch alle Signaturen). Nicht in diesem
  Schritt.
- **Wolkenschatten-Map** (läuft im Shadow-Command-Buffer): Shadow p50 bei `crop` 0,846 / 0,850
  gegen 0,862 bis 0,871 ms bei „shared“, also ~0,02 ms. Die 512²-Map hängt nicht nennenswert am
  Fußabdruck.

### Himmel-only-Szene (nur zur Vollständigkeit)

`skyonly.hescene` (ganzes Bild Himmel mit Wolken), sieben Läufe shared/crop/private im Wechsel
(`raw-b7/SK-*.summary.json`). Wie schon in A4 bimodal (Drawable-Pacing bei gesperrtem Bildschirm,
bis 64 Hänger pro 600 Frames). Scene p50 schwankt bei „shared“ zwischen 4,92 und 5,43 ms, die
Varianten zwischen 3,21 und 4,74 ms, aber mit Hänger-Zahlen, die die p50 verschieben. Nicht
ausgewertet.

### Xcode-/xctrace-GPU-Zähler

Zwei Versuche, beide ohne Zählerwerte:
`xctrace record --instrument 'Metal GPU Counters' --instrument 'Metal Application' --launch` und
`--template 'Game Performance Overview'`. Beide enden mit rc = 54. Die Encoder-Liste ist
aufgezeichnet (3,5 MB), die Tabellen `gpu-counter-info`, `gpu-counter-value` und
`metal-gpu-counter-intervals` sind leer (TOC: `raw-b7/xctrace-gpu-counters-toc.xml`). Vermutlich
braucht das Instrument ein in Instruments gewähltes Zählerprofil oder einen entsperrten
Bildschirm. Limiter-Anteile und Cache-Fehlraten fehlen deshalb. Der Befund oben stützt sich auf
die kausale Sonde (Pass-Zeitstempel), nicht auf Zähler.

## Korrekturen an der Synthese (B7-Zeile)

- Wirkung „unbekannt“ → **Sampling ~1,5 ms Scene-GPU (Landschaft, 2840×1528, gesperrt) hängen am
  Fußabdruck; Erzeugung 10,7 s CPU beim Start (behoben).**
- Aufwand „S (`Private` + Blit-Upload)“ → **wirkungslos auf die GPU-Zeit**, gemessen.
- „M (Mips)“ → **+0,8 bis +1,2 ms**, gemessen. Verwerfen.

## Offen (Kandidaten für einen eigenen Schritt, nicht umgesetzt)

- R und G als zwei `R16Unorm`-Volumen: verlustfrei, erwartbar ≤ ~0,3 ms (nach der `rg8`-Sonde).
- Den Fußabdruck wirklich verkleinern (kleineres Volumen oder weniger hohe Oktaven in der Ferne)
  holt den Großteil der ~1,5 ms, ändert aber die Optik und braucht ein eigenes Sicht-A/B.
- GPU-Zähler mit entsperrtem Bildschirm bzw. in Instruments mit gewähltem Zählerprofil, um
  Limiter und Cache-Fehlrate direkt zu sehen.
- Erzeugungszeit auf GL/D3D/Vulkan nicht gemessen (gleicher Builder, gleiche Wirkung erwartet).
