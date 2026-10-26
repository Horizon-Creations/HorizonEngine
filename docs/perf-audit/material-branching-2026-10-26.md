# Material-If als echter Zweig: Wasser und Auto-Landscape (26.10.2026)

Bezug: Hive-Thema 235 Schritt 2 (Wasser aus der Nähe), Thema 239 (GPU-Audit, Metal-Teil),
`docs/perf-water-cost-2026-10-21.md` (Hebel „Unterseite: das If rechnet beide Seiten“),
`frame-parallelism-audit-2026-10-26.md` §2.4 (Catania: der Scene-Pass ist 11,7 von 14,8 ms GPU).

## 1. Was sich ändert

**Codegen (`MaterialGraph.cpp`).**
- Das If-Node war bisher `mix(no, yes, step(0.5, cond))` und rechnete beide Seiten für jedes Pixel.
- Jetzt bestimmt `computeBranchGuards` für jeden Knoten der Wurzel-Szene, unter welcher Seite welchen Ifs er allein gebraucht wird:
  - Bedingung ist, dass alle Verbraucher auf dem Weg zum Output sich einig sind.
  - Diese Knoten laufen in `if (cond >= 0.5) {...} else {...}`.
  - Ihre Variablen werden vor dem Block deklariert. So kann ein zweiter Block auf dieselbe Bedingung sie weiter lesen.
- **In einen Zweig dürfen nur:**
  - Rechen- und Rauschknoten, Konstanten, Parameter und Eingänge, Static Switch und Reroute;
  - die vier Bombing-Fetches (`textureGrad`). Ihr Gitter, die Slices und die Bildschirm-Gradienten (`dFdx(uv)`, `dFdx(vWorldPos)` für `hePerturbNormalD`) laufen davor.
- **Nicht in einen Zweig:**
  - ein Knoten, der einen unreinen Knoten liest (`texture()` mit implizitem LOD, Funktion). Begründung: macOS-OpenGL hat einen solchen Fetch sonst mit in den Zweig gezogen, im Plain-Modus der Landschaft 31/255 an Zweigrand-Pixeln;
  - Funktions-Scopes, die WPO- und die Displacement-Stufe;
  - ein selbst bewachtes If. Das bleibt `mix`.
- Ein Graph ohne verschiebbaren Rechenknoten behält exakt seinen alten Text.

**Wasser (`mat_gen`).**
- Unterseite und Oberseite liegen jetzt in getrennten Zweigen auf `below`.
- Jede der vier Chop-Schichten läuft hinter `If(fade > 0)`: Jenseits ihres Fades wird das `FbmGrad` nicht mehr gerechnet.

**Auto-Landscape (`AutoLandscapeMaterial.cpp`).**
- Erde, Fels und Schnee lesen ihre 9 gebombten Fetches (Albedo, Normal, Maske, je mal 3 Hexes) nur noch dort, wo ihr Gewicht größer als 0 sein kann.
- Die Bedingung ist eine **obere Schranke** des automatischen Gewichts:
  - der Rampen-Eingang mit der größtmöglichen Höhenverschiebung, also Height Blend mal Skala, als dasselbe Produkt in derselben Reihenfolge wie `heightBias`;
  - oder ein gemaltes Gewicht;
  - beim Schnee zusätzlich die Wetter-Schneedecke größer als 0 (`MF_WeatherSnow` ist bei Decke 0 immer 0).
- Wo die Schranke nicht greift, ist das Gewicht exakt 0. Deshalb ändert kein Pixel.
- Gras bleibt immer an.
- Die Maskenansichten bleiben ohne Schranken, sie lesen keine Gewichtskarte.

## 2. Messung (Metal, M5, entsperrt, Netzteil, ruhige Maschine, gleicher Takt laut Tonemap)

**Wasser.** `scripts/perf/water_matrix.py fix --editors base=…,branch=… --plain --detailed --reps 3`, Scene-Pass p50 in ms, 3200×1800, Median aus 3 Wechselläufen:

| Kamera | release/0.7.0 | Zweig (Unterseite) | Δ |
|---|---|---|---|
| down3 (3 m, steil nach unten) | 8,92 | 8,44 | −0,5 (−5 %) |
| near3 (3 m darüber) | 5,74 | 5,24 | −0,5 (−9 %) |
| under (1,5 m darunter) | 5,87 | 4,83 | −1,0 (−18 %) |

Chop-Schichten hinter ihrem Fade, Gruppe `chop`, Asset-A/B auf dem Zweig-Binary:

| Kamera | vorher | nachher | Δ |
|---|---|---|---|
| mid (30 m) | 4,54 | 3,99 | −0,55 (−12 %) |
| far (130 m) | 2,53 | 2,38 | −0,15 |
| near3 | 5,59 | 5,43 | −0,16 |
| down3 | 8,03 | 8,27 | +0,24 (Streubereich der Einzelläufe 7,7..8,3 gegen 7,8..8,05) |

**Auto-Landscape.**
- Aufbau: `gen_autoland_scene.py --extent 512 --meshes none`, RenderScale 2, `--detailed`, leeres `HOME`, ABAB 3x, Treiber im Scratch (`land_ab.sh`).
- Scene-Pass p50 in ms:

| Kamera | release/0.7.0 | Zweig | Δ |
|---|---|---|---|
| hills (15 m, Blick über die Hügel) | 15,26 | 12,18 | −3,1 (−20 %) |
| slope (25 m, Hang) | 16,31 | 12,25 | −4,1 (−25 %) |

## 3. Bild

`he_shot`-Zeugen, `HE_SKY_TIME=30`, AA aus, alt (release/0.7.0 @ af3b64dd) gegen neu, je auf Metal und OpenGL. Grundrauschen: Derselbe Build zweimal ist bitgleich.
- **Wasser** (Oberseite, Draufsicht, unter Wasser, 400 m weit): höchstens 1/255 an einzelnen Kanälen, mittel 0,0000.
- **Auto-Landscape** `AUTOLAND=1/masks/ground/normal/surface/nobomb` mit zwei Kameras, Deferred, Schnee-Plateau:
  - Die Masken- und Ground-Ansichten sind bitgleich, ebenso das Lit-Bild der ersten Kamera und das Schnee-Plateau.
  - Der Rest weicht um höchstens 1/255 ab, auf GL einmal um 2/255.
  - Ursache: `hePerturbNormalD` und Zuweisung statt `mix` rechnen in anderer Reihenfolge.

## 4. Nicht geprüft

- D3D11, D3D12 und Vulkan: Text und Kreuzkompilat sind durch `test_material_branching` (HLSL, Metal, GLSL 4.10) und `test_material_graph` (alle Knoten nach HLSL/SPIR-V) geprüft, das Bild auf Hardware nicht.
- FXC-Warnungen zu `[branch]` und Uniformität: nicht gesehen.
- Catania-Startszene nicht separat gemessen. Ihr Gelände ist flach (`heightScale 0`), also eher Gras als Fels und Schnee.

## Handoff

- **Topic / step:** Thema 235 Schritt 2 (Wasser) und Thema 239 Metal-Teil (Landschaft), Zweig `claude/perf-material-branching` ab release/0.7.0 @ af3b64dd.
- **Done:** Codegen-Zweige (`computeBranchGuards`, `placeDecl`, `flushGuards`, Bombing-Vorspann, `hePerturbNormalD`); Wasser-Unterseite und Chop-Fades; Landschafts-Schranken; Assets `Water.hasset` und `M_AutoLandscape.hasset` neu erzeugt; `water_matrix.py --editors/--under-load`, Gruppe `chop`; `tests/test_material_branching.cpp`.
- **Verified:** §2 und §3; `test_material_branching` 9/9, `test_material_graph` 105/105, `test_mcp_tools_material`, `test_auto_landscape_variants` grün.
- **Not verified:** §4.
- **Decisions taken:** Schranken statt exakter Gewichte (bitgleich, kein Look-Risiko); keine Texturen in Engine-Materialien (Chop bleibt prozedural, Regel in `mat_gen`).
- **Next steps:** 1. Windows (NN-WS03): Bildvergleich D3D11/D3D12/Vulkan für Wasser und Landschaft, wie §3. 2. Gras überspringen, wo Fels oder Schnee voll deckt (Schranke von unten). 3. Ein vom Nutzer gebautes Material mit If profitiert automatisch; im Material-Editor-Tooltip des If erwähnen.
