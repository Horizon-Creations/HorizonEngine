# Unterwasser-Post-Processing: Abnahme (Thema 185, Schritt 4)

Stand 12.10.2026, Branch `claude/unterwasser-post-processing` (PR #140). Gemessen auf dem Mac (Apple M5,
Release, shaderc ON), Metal und OpenGL. Die Zahlen kommen aus `scripts/underwater_acceptance.py`; wer ein
anderes Backend abnimmt, ruft es mit `--rhi` auf und bekommt dieselben Tabellen.

## Was womit belegt ist

| Backend | Nebel | Kaustik | Belegt durch |
|---|---|---|---|
| Metal | ja (neu in Schritt 4) | ja | Editor-Bilder, A/B, Pass-Zeit (diese Seite) |
| OpenGL | ja (neu in Schritt 4) | ja | Editor-Bilder, A/B (Pass-Zeit nicht belastbar, siehe unten) |
| D3D11 | ja | ja | nur Compile in CI. Nie gesehen. |
| D3D12 | ja | ja (neu in Schritt 4) | nur Compile in CI (Text-Patch nach dem Nebel-Block). Nie gesehen. |
| Vulkan | ja (Rekonstruktionsmatrix korrigiert) | ja (neu in Schritt 4) | nur Compile/Validator in CI. Der lavapipe-Job hat keinen Unterwasserfall, der Pass lief nie. |

**Die Hardware-Abnahme von D3D11, D3D12 und Vulkan steht aus** (Schritt 5, NN-WS03). Software-Rasterizer
(WARP, lavapipe) und CI-Compile ersetzen sie nicht.

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

## 4. Durchqueren der Wasserlinie: NICHT sprungfrei

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

## 7. Offen

- Hardware-Abnahme D3D11/D3D12/Vulkan (Schritt 5): `python scripts/underwater_acceptance.py --rhi D3D12 --out shots/d3d12`,
  danach `--compare shots/gl shots/d3d12`. Erwartung nach den Mac-Zahlen: unter Wasser Abweichung zu OpenGL unter 1.
  Dazu die Pass-Zeit (PIX/Nsight oder `he_perf_capture.py`) und der Wasserlinien-Sprung.
- Ein Unterwasserfall im lavapipe-Job (`scripts/he_vk_imagetests.py`): `WATERLAKE=bowl` mit `WATERHIDESHEET` (noch nicht
  vorhanden), A/B `UWFOG=0 UWCAUSTICS=0` gegen Nebel/Netz; er wuerde als erstes die neuen Vulkan-Pipelines und die
  Layout-Wechsel des Nebel-Blocks unter Validation laufen lassen.
- Wasserlinie (Abschnitt 4), Netzaussehen (bei Skala 1 weiche Einzelflecken, Skala 3 feiner), Standard-Truebung
  (0,45 je Meter: ab etwa 5 m 90 % Nebel, das Netz reicht 14 m), Lichtschaechte (volumetrisch, nie gebaut).
