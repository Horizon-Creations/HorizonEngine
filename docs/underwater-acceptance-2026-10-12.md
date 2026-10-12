# Unterwasser-Post-Processing: Abnahme (Thema 185, Schritt 4)

Stand 12.10.2026, Branch `claude/unterwasser-post-processing` (PR #140). Gemessen auf dem Mac (Apple M5,
Release, shaderc ON), Metal und OpenGL. Die Zahlen kommen aus `scripts/underwater_acceptance.py`; wer ein
anderes Backend abnimmt, ruft es mit `--rhi` auf und bekommt dieselben Tabellen.

## Was womit belegt ist

| Backend | Nebel | Kaustik | Belegt durch |
|---|---|---|---|
| Metal | ja (neu in Schritt 4) | ja | Editor-Bilder, A/B, Pass-Zeit (diese Seite) |
| OpenGL | ja (neu in Schritt 4) | ja | Editor-Bilder, A/B (Pass-Zeit nicht belastbar, siehe unten) |
| D3D11 | ja | ja | Schritt 5: echte Hardware (NN-WS03, RTX 4070), Bilder, A/B, Pass-Zeit (Abschnitt 8) |
| D3D12 | ja | ja (neu in Schritt 4) | Schritt 5: Hardware mit Debug-Layer, 0 Fehler, Bilder, A/B, Pass-Zeit (Abschnitt 8) |
| Vulkan | ja (Rekonstruktionsmatrix korrigiert) | ja (neu in Schritt 4) | Schritt 5: Hardware mit Validation, 0 Fehler, Bilder, A/B. Pass-Zeit nicht aufloesbar (Abschnitt 8) |

Die Hardware-Abnahme von D3D11, D3D12 und Vulkan ist in Schritt 5 gelaufen (Abschnitt 8). Dort ist auch der
Wasserlinien-Sprung aus Abschnitt 4 behoben, auf allen fuenf Backends. Metal ist dabei nur geschrieben und von
der CI kompiliert, nicht gesehen.

Ctest dieses Stands: 275 Tests, 100 % bestanden (273 gelaufen, 2 Groessentests absichtlich uebersprungen),
542 s, Release mit shaderc ON. `validate_embedded_shaders.py --glsl-only`: 58 von 58.

## Aufbau der Bilder

`HE_DUMP_WATERLAKE=bowl` (ein echter Wasserkoerper, Flaeche bei Welt-Y 302, der Boden steigt nach +Z), Kamera
bei (0, 302 - Tiefe, 28), `YAW=180 PITCH=-20`, `TOD=0.45`, deferred, `AA=0 BLOOM=0`, `HE_SKY_TIME=5`, 1280x720.
Neue Schalter (auf jedem Koerper des Witness, ueber dieselben Setter wie das Water-Tab):

- `HE_DUMP_UWFOG="dichte[,falloff]"` und `HE_DUMP_UWCAUSTICS="intensitaet[,skala[,tempo[,tiefe]]]"`
- `HE_DUMP_SAVESCENE=<datei.hescene>` schreibt die Witness-Welt, damit der echte Editor-Loop sie oeffnen kann
  (`scripts/he_perf_capture.py --scene`).

## 1. Nebel und Netz, Metal gegen OpenGL (2 m tief, gegen "beides aus")

Mittlere Abweichung (von 255) zur Aufnahme mit `UWFOG=0 UWCAUSTICS=0`:

| Variante | OpenGL | Metal |
|---|---|---|
| nur Nebel (Dichte 0,06) | 23,226 | 23,225 |
| nur Netz (Standard 0,55) | 0,638 (5,74 % der Pixel > 2) | 0,637 (5,74 %) |
| Standardwerte (Dichte 0,45 + Netz) | 131,436 | 131,519 |

Beide Stufen sind im echten Editor sichtbar und stimmen zwischen den beiden Backends auf etwa ein Promille ueberein.
Bilder: `docs/images/underwater-accept-gl-both-off.png`, `...-gl-net-only.png`, `...-gl-defaults.png`,
`...-metal-defaults.png`.

Gleiche Kamera und Parameter, Metal gegen OpenGL pixelweise: Tiefe 0,4 m 0,650 (7,4 % > 2), 2 m 0,239, 4 m 0,120,
Standard 3 m 0,115, 0,5 cm unter der Flaeche 0,967, 30 cm tief 0,406, trocken 3 m darueber 0,603.

## 2. Parametrisierung des Netzes (OpenGL, Nebel 0,06)

- Intensitaet 2,0 gegen "Netz aus": 1,778 (6,3 % der Pixel). Standard 0,55: 0,638.
- Skala 3 (Intensitaet 0,8): 14,813 (39 % der Pixel), ein feineres Netz.
- **Skala 0,5: 0,000, das Netz verschwindet ganz.** Die Kruemmung der Wellen sinkt mit der Skala, `1/|det| - 2`
  erreicht nie die Fokussierschwelle. Unter etwa 0,7 gibt es mit den Standard-Wellenzuegen kein Netz.

## 3. Einblenden des Netzes an der Wasserlinie (OpenGL, nur Netz gegen beides aus)

| Tiefe | 0,5 cm | 10 cm | 25 cm | 50 cm | 100 cm |
|---|---|---|---|---|---|
| mittlere Abweichung | 0,000 | 0,038 | 0,139 | 0,277 | 0,405 |

Das Netz blendet wie entworfen ein (0 auf der Linie, Plateau ab etwa 0,5 m), kein Springen.

## 4. Durchqueren der Wasserlinie: NICHT sprungfrei (Stand Schritt 4, behoben in Schritt 5, siehe 8.3)

Standardparameter, Kamerahoehe relativ zur Flaeche, Abweichung zwischen aufeinanderfolgenden Aufnahmen:

| Schritt | Kamera | OpenGL | Metal |
|---|---|---|---|
| +30 -> +10 cm | 20 cm | 24,97 | - |
| +10 -> +3 cm | 7 cm | 95,26 | - |
| +3 -> +0,5 cm | 2,5 cm | 22,95 | - |
| **+10 -> +0,5 cm** | | 100,65 | 44,60 |
| **+0,5 cm -> -0,5 cm (die Linie)** | **1 cm** | **108,73** | **108,92** |
| -0,5 -> -3 cm | 2,5 cm | 6,94 | - |
| -3 -> -10 cm | 7 cm | 11,97 | - |
| -10 -> -30 cm | 20 cm | 10,54 | - |
| -0,5 -> -30 cm | | 23,55 | 23,63 |

Unter Wasser sind die Schritte glatt (7 bis 12). Ueber die Linie springt das Bild um 109 von 255, auf beiden
Backends gleich. Bilder: `...-waterline-above-10cm-gl.png` (Wasserflaeche von oben, dunkelblau) gegen
`...-waterline-below-0.5cm-gl.png` (von knapp darunter: der Boden in der Truebung, tuerkis).

Ursachen (beim Lesen der Bilder und des Codes, nicht einzeln isoliert):

1. Der Pass laeuft nur mit `underwaterCamera.wet`. Von oben sieht man die Wasserflaeche (Wassermaterial), von
   unten den Boden mit Nebel ueber der ganzen Strecke Kamera -> Boden. Das sind verschiedene Bildinhalte.
2. Naehe der Flaeche: Knapp ueber der Flaeche schneidet die Near-Plane die Wasserflaeche ab, man sieht den
   hellen, unnebligen Boden. Bei OpenGL passiert das zwischen 3 und 10 cm Hoehe, bei Metal schon bei 10 cm (dort
   fehlt dann das untere Drittel der Flaeche, Abweichung Metal gegen OpenGL in dieser Aufnahme 56). Metal rastert
   mit der GL-Projektion in einem [0,w]-Clip (Kommentar in `RenderExtractor.cpp`), seine wirksame Near-Plane ist
   deshalb etwa doppelt so gross wie die von OpenGL. Das ist alt, aber es verbreitert den Sprung.
3. Der Nebelton (Schritt 2: `shallowColor` 0,045/0,20/0,22) ist nicht auf das Material der Wasserflaeche abgestimmt.

Vorschlag (eigener Schritt, alle fuenf Backends): `queryUnderwaterCamera` loest Ton und Dichte auch fuer eine
trockene Kamera knapp ueber einem Koerper auf, und der Nebel laeuft dort fuer Pixel unterhalb der Ebene (Strecke
Kreuzungspunkt -> P). Dazu den Nebelton aus dem Wassermaterial ableiten oder die Flaeche von unten doppelseitig
mit demselben Nebel zeichnen.

## 5. Pass-Zeit

**Metal** (`he_perf_capture.py --detailed`, versteckter Editor, 3200x1800, Szene aus `HE_DUMP_SAVESCENE`, gleiche
Kamera, 300 Frames nach 200 Warmup): Der Pass kodiert im Bloom-Command-Buffer, hat im Detail-Modus keine eigene Zeile.

| Zeile (p50, ms) | beides aus | Standardwerte |
|---|---|---|
| Bloom (enthaelt den Pass) | 1,462 | 2,705 (+1,243) |
| Scene / SSAO / Shadow / Tonemap / Present | 5,268 / 2,911 / 1,292 / 0,739 / 1,475 | 5,251 / 2,903 / 1,268 / 0,727 / 1,416 |
| GPU-Frame gesamt | 13,17 | 14,40 (+1,23, +9 %) |

Also etwa 1,2 ms bei 5,76 MP, 0,2 ms je Megapixel. Auf 1280x720 etwa 0,2 ms. Die CPU-Zahlen sind unbrauchbar:
der Mac lief mit Last 15 bis 38 (andere Bienen bauten mit).

**OpenGL**: Der Shader allein (CGL-Harness, derselbe Quelltext, 3200x1800): Netz 0,21 ms, Nebel 0,10 ms, beides 0,20 ms
je Draw. Im Editor-Loop gibt es auf macOS-GL keine GPU-Pass-Zeiten. Die FPS-Paare (an 36,7 / 41,0 / 42,3, aus
76,6 / 42,7 / 75,8) schwanken unter der Last um das Doppelte und trennen Pass und Rauschen nicht. **Nicht
belastbar, auf ruhigem Rechner wiederholen.**

D3D11/D3D12/Vulkan: nicht gemessen.

## 6. Funde dieses Schritts (alle behoben, ausser Abschnitt 4)

1. CI rot (Lauf 38250184755): `validate_embedded_shaders.py` uebersetzte die Kaustik-Shader mit dem Platzhalter
   `@TRAINS@` (FXC "unexpected token '@'", Metal "expected expression"). `extract()` setzt jetzt zwoelf Zahlen ein.
2. **Metal las eine flache Tiefe**: `m_hdrDepth` wurde nur gespeichert, wenn DoF, Bewegungsunschaerfe oder TAA an
   waren. Auf einer Tile-GPU erreicht ein DontCare-Attachment nie den Speicher. `underwaterPostWanted()` ist jetzt
   die eine Definition fuer Gate und Tiefen-Store. Die Aufnahmen entstanden mit der Korrektur, ein Vorher-Bild
   gibt es nicht.
3. **Vulkan-Nebel rekonstruierte falsch** (Schritt 2): Matrix `inverse(P*V)` statt `inverse(kVulkanClipFix*P*V)`,
   mit der die Szene gezeichnet wird und Decals/SSR/Deferred-Resolve rekonstruieren. CPU-Nachrechnung an drei Punkten:
   8 bis 14 m daneben. Eine Zeile korrigiert, nur statisch geprueft.
4. Der Nebel fehlte auf OpenGL und Metal, die Kaustik auf D3D12 und Vulkan. Beides ergaenzt.
5. Die Vulkan-Kaustik haelt den Push-Block bei 128 Byte (das Minimum, das jedes Geraet garantiert, lavapipe hat genau
   das): die Tiefenkonstanten sind eingebacken, die zwoelf Wellenzug-Zahlen stehen als Literal im Shader, ein Test
   vergleicht sie mit `HE::causticsWaveTrainsList()`.

## 7. Offen (Stand nach Schritt 5)

- ~~Hardware-Abnahme D3D11/D3D12/Vulkan~~ und ~~Wasserlinien-Sprung~~: erledigt in Schritt 5 (Abschnitt 8).
- Metal mit dem Fix aus Schritt 5 auf dem Mac ansehen: `--rhi Metal`, Tabelle 2 (die Linie) und Tabelle 5. Erwartung wie
  D3D: Metal schneidet wie D3D bis etwa +10 cm ab, also Tabelle 5 bei +10 cm ungleich 0, ab +30 cm 0,000.
- Farbton der beiden Haelften knapp ueber der Linie (8.3): oben die Wasserflaeche (blau), unten der Nebel (tuerkis).
  Die Kante wandert stetig, ist aber sichtbar. Abhilfe waere der Nebelton aus dem Wassermaterial (Vorschlag aus 4).
- Pass-Zeit auf Vulkan und OpenGL: mit der Ganzframe-Zeit nicht aufloesbar (8.4). Braucht eigene Zeitstempel um den
  Unterwasser-Block oder PIX/Nsight.
- Ein Unterwasserfall im lavapipe-Job (`scripts/he_vk_imagetests.py`): `WATERLAKE=bowl` mit `WATERHIDESHEET` (noch nicht
  vorhanden), A/B `UWFOG=0 UWCAUSTICS=0` gegen Nebel/Netz; er wuerde als erstes die neuen Vulkan-Pipelines und die
  Layout-Wechsel des Nebel-Blocks unter Validation laufen lassen.
- Netzaussehen (bei Skala 1 weiche Einzelflecken, Skala 3 feiner), Standard-Truebung
  (0,45 je Meter: ab etwa 5 m 90 % Nebel, das Netz reicht 14 m), Lichtschaechte (volumetrisch, nie gebaut).

## 8. Windows-Hardware-Abnahme (Schritt 5, NN-WS03)

12.10.2026, NN-WS03 (NVIDIA RTX 4070), Release, privater Baum `C:\hw185` mit eigenem `DEPLOY_DIR`, alle vier
Windows-Backends auf derselben Karte. D3D12 lief mit Debug-Layer + DRED (`HE_GPU_DEBUG=1`), Vulkan mit
Validation-Layer (im Backend fest an). Ueber alle 37 Aufnahmen je Backend: **0 `[ERROR]`**. D3D12 meldet je Lauf nur
zwei allgemeine Warnungen (ein Pixel-Shader mit vierter Ausgabe in einem PSO mit drei Zielen, ein Clear-Wert). Beide
betreffen die Unterwasser-Passes nicht, sie schreiben nur `SV_Target0`. D3D11 hat keinen Debug-Layer, dort zaehlen nur
die Bilder.

Befehle (Windows, `--jobs 1` ist dort Standard):

    python scripts/underwater_acceptance.py --rhi D3D11 --out C:/hw185/shots/fix_d3d11 --editor C:/hw185/deploy/Editor/HorizonEditor.exe
    python scripts/underwater_acceptance.py --compare C:/hw185/shots/fix_gl C:/hw185/shots/fix_d3d11
    python scripts/underwater_acceptance.py --rhi D3D12 --out C:/hw185/time6_d3d12 --gputime "600,3200x1800" --rounds 6 --editor ...

### 8.1 Die Passes laufen auf allen vier Windows-Backends (Tabelle 1, 2 m tief)

| gegen "beides aus" | OpenGL (Win) | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| nur Nebel | 23,205 | 17,483 | 17,483 | 17,478 |
| nur Netz | 0,634 | 0,234 | 0,234 | 0,233 |
| Standardwerte | 131,474 | 135,385 | 135,385 | 135,426 |
| Standardwerte zweimal (Rauschboden) | 0,000 | 0,000 | 0,000 | 0,000 |

- Windows-GL trifft die Mac-Zahlen aus Abschnitt 1 (131,47 gegen 131,44, Linie 108,8 gegen 108,7).
- D3D12 ist pixelgleich zu D3D11 (hoechstens 1/255 je Pixel, alle Aufnahmen).
- Vulkan weicht unter Wasser von D3D11 um 0,06 bis 0,6 ab. Die Rekonstruktionskorrektur aus Abschnitt 6.3 haelt also
  auf Hardware. Der alte Fehler (8 bis 14 m daneben) waere hier nicht zu uebersehen.
- D3D/Vulkan liegen gegen GL bei etwa 25, auch mit beiden Passes aus und trocken 3 m ueber dem See. Das ist die
  bekannte hellere D3D/Vulkan-Szene (der Boden ist fast weiss), nicht der Pass. Auf dem helleren Boden saettigt das
  Netz, daher 0,23 statt 0,63.

### 8.2 Ursache des Sprungs auf D3D: die Near-Plane

D3D11/D3D12 rastern wie Metal die GL-Projektion in einen [0,1]-Clip. Die wirksame Near-Plane ist doppelt so weit
weg, die Wasserflaeche wird bis etwa +10 cm abgeschnitten (GL/Vulkan: zwischen +3 und +10 cm). Vorher, D3D11 gegen GL
bei +10 cm: 76. Darunter sah man den hellen, ungenebelten Boden, einen Zentimeter tiefer denselben Boden voll im Nebel.

### 8.3 Der Fix und die Linie danach

Commit e51e44c3. `queryUnderwaterCamera` gibt einer TROCKENEN Kamera bis 0,5 m ueber einer Flaeche den Nebelton
(shallowColor) und die Dichte des Koerpers mit, bis 1 m ausgeblendet (`HE::fogWaterlineBand`). `wet` und das Netz
bleiben, wie sie waren. Die Nebel-Shader aller fuenf Backends nebeln eine trockene Kamera nur dort, wo der
Near-Plane-Punkt des Pixels schon unter der Ebene liegt. Er entsteht aus d = 0 durch die eigene Tiefenabbildung des
Shaders. Das sind genau die Pixel, an denen die Flaeche weggeschnitten ist; dort bekommt der Boden die Strecke vom
Ebenenschnitt bis zum Pixel, wie von knapp darunter.

Erster Versuch ohne diesen Test: Er hat den ganzen See von oben getoent, 28 bis 58 von 255 bei +30 bis +80 cm. Die
transluzente Wasserflaeche steht NICHT im Tiefenpuffer, den die Passes lesen. Das ist auch eine hive-Lehre.

Schritt ueber die Linie (+0,5 -> -0,5 cm, mittlere Abweichung von 255):

| | OpenGL | D3D11 | D3D12 | Vulkan |
|---|---|---|---|---|
| vorher | 108,8 | 122,2 | 122,2 | 121,9 |
| **nachher** | **7,6** | **4,8** | **4,8** | **8,1** |
| Forward-Pfad (`RENDERPATH=0`), nachher | 7,6 | 4,6 | 4,6 | 7,8 |

Zum Vergleich: Unter Wasser liegen die Nachbarschritte (2,5 bis 20 cm) bei 7 bis 12.

Die Schritte knapp ueber der Linie, nachher (vorher in Klammern):

| Kamera | OpenGL | D3D11/D3D12 | Vulkan |
|---|---|---|---|
| +30 -> +10 cm | 25,0 (25,0) | 37,3 (80,4) | 22,9 (22,9) |
| +10 -> +3 cm | 46,2 (95,3) | 22,4 (51,6) | 32,8 (100,1) |
| +3 -> +0,5 cm | 19,0 (23,0) | 13,2 (8,2) | 18,2 (26,8) |

Was bleibt, ist die Kante, an der die Near-Plane die Flaeche abschneidet. Darueber liegt die Flaeche von oben (blau),
darunter der Boden im Nebel (tuerkis). Die Kante wandert mit der Hoehe stetig ueber das Bild, wie bei einer Linse, die
halb im Wasser steckt. Das ganze Bild schlaegt nicht mehr um. Bild:
`docs/images/underwater-waterline-d3d11-before-after.png` (oben vorher, unten nachher; +3 / +0,5 / -0,5 cm).

Von oben bleibt der See unveraendert. Tabelle 5: Standardwerte gegen dieselbe trockene Kamera mit `UWFOG=0`, das
ist das Bild ohne Band.

| Kamera | OpenGL | D3D11/D3D12 | Vulkan |
|---|---|---|---|
| +80 / +60 / +30 cm | 0,000 | 0,000 | 0,000 |
| +10 cm | 0,000 | 47,6 (D3D schneidet hier noch ab) | 0,000 |
| +3 / +0,5 cm | 69,0 / 101,3 | 98,4 / 117,7 | 76,1 / 114,0 |

Unit-Tests: 29 von 29 der Faelle `Underwater*`. Zwei sind neu: die Band-Funktion, und die Dichte ist auf beiden
Seiten der Linie gleich, hoch ueber dem See und neben ihm aber 0. `validate_embedded_shaders.py --no-msl`: 113 von 113.

### 8.4 Pass-Zeit

Neuer Dump-Schalter `HE_DUMP_GPUTIME="frames[,WxH]"` (EditorApplication, Commit bb5aadbf). Nach der Aufnahme rendert
er N Frames bei WxH mit Live-GPU-Zeitmessung und protokolliert die GPU-Zeit fuer das ganze Frame. D3D11, D3D12 und
Vulkan haben keine Zeilen pro Pass. Der Pass ist also der Unterschied zwischen "beides aus" und Standardwerten, 2 m
tief, 3200x1800, 600 Frames, sechs Runden im Wechsel, ohne Debug-Layer.

| Backend | beides aus (Median der Laeufe) | Standardwerte | Pass |
|---|---|---|---|
| D3D11 | 6,80 ms (6,62..6,92) | 6,95 ms (6,90..7,06) | **+0,15 ms** |
| D3D12 | 7,07 ms (5,98..7,07)* | 7,22 ms (7,02..7,23) | **+0,15 ms** |
| Vulkan | 10,9..23,2 ms | 10,5..26,0 ms | nicht aufloesbar |
| OpenGL | 16,2..18,3 ms | 16,1..17,5 ms | nicht aufloesbar |

\* D3D12 ist innerhalb eines Laufs sehr eng (Minimum etwa gleich Median), springt aber zwischen zwei Taktstufen
(5,98 / 7,07 ms). Der Median ueber die Laeufe faellt in die obere.

Also etwa 0,15 ms bei 5,76 MP auf der RTX 4070, rund 0,03 ms je Megapixel. Bei 1080p sind das etwa 0,05 ms, bei
1280x720 unter dem Rauschen. Zum Vergleich Metal (M5): 1,2 ms.

Vulkan streut innerhalb eines Laufs zwischen 8 und 34 ms, GL zwischen Laeufen um 2 ms. Beides ist groesser als der Pass.
Bei Vulkan ist der Validation-Layer im Backend fest an. `VK_LOADER_LAYERS_DISABLE=~all~` schaltet ihn ab, die Instanz
startet trotzdem, aber die Ganzframe-Zeit blieb bei 20 bzw. 25 ms. GL liefert Zeilen pro Pass nur waehrend einer
Profiler-Aufnahme, im Dump-Modus nicht. Die Shader-Arbeit ist auf allen Backends dieselbe (ein bzw. zwei
Fullscreen-Draws); eine eigene Messung fuer Vulkan und GL fehlt.
