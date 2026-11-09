# Wasser-Strömung und Wasserfall: Backend-Abnahme (Thema 232, Schritt 4 der Queen-Liste)

09.11.2026, MacBook Air M5, Release-Build von `8ce5c03e9`. Entwurf und Schritte 1 bis 5: `docs/water-flow-2026-11-09.md`.

**Umfang laut Queen:** NN-WS03 (Windows) ist offline. **D3D11, D3D12 und Vulkan sind NICHT verifiziert.** Geprüft sind nur Metal und OpenGL (macOS, GL 4.1 über Metal).

## Ergebnis

| Punkt | Metal | OpenGL | D3D11 / D3D12 / Vulkan |
|---|---|---|---|
| Build `HorizonEditor` + `he_tests` | grün (`cmake --build`, nichts mehr zu bauen, Binaries von 05:52) | (gleicher Build) | nicht verifiziert |
| `ctest` komplett | **365/365 grün** (198 s) | – | nicht verifiziert |
| Kaskaden-Zeuge (`HE_DUMP_WATERLAKE=cascade`) | Bild, 31 Körper, 30 Terrassen, Material `600,3` WaterFlow, Shader bereit | Bild, dieselbe Zählung, `glGetError=0` | nicht verifiziert |
| Perf Standwasser gegen Messung vom 21.10. | gleich (s. u.) | +2,1 ms gegen ohne Wasser, nicht schlechter als Metal | nicht verifiziert |
| Perf Flow-Variante gegen Standwasser | +0,0 bis +0,4 ms | 0 ms (Rauschen) | nicht verifiziert |

## Visueller Vergleich

`docs/water-flow-cascade-metal-2026-11-09.png` und `docs/water-flow-cascade-opengl-2026-11-09.png`: Befehl wie Schritt 5
(sauberes `HOME`, `HE_DUMP_WATERLAKE=cascade HE_DUMP_WATERSLOPE=0.8 SKYTEST=1 TOD=0.5 DAYNIGHT=0 SETTLEMS=1500 FRAMES=6`,
`CAMX=-24 CAMY=292 CAMZ=0 PITCH=15 YAW=90`, `HE_DUMP_RHI=Metal|OpenGL`). Beide Backends zeigen dieselbe Geometrie: dreißig Stufen, durchgehender
Vorhang zwischen den Stufen, senkrechte Streifen mit der Strömung, Schaumvorfeld am Fuß. Unterschiede: GL ist etwas dunkler und der Himmel
weicht ab (kein GI, 8-Lichter-Fenster auf GL < 4.3, andere Wolkenberechnung); das sind die bekannten Backend-Unterschiede, nicht dieses Features.
Kein md5-Vergleich (Himmel und Beleuchtung unterscheiden sich je Backend, ein Gleichheits-Test wäre sinnlos).

## Performance gegen `docs/perf-water-cost-2026-10-21.md`

Methode wie dort: `scripts/perf/water_matrix.py live --plain --home <leer> --caffeinate --detailed`, 400 Frames, 2840x1528, Display wach, Bildschirm gesperrt
(`conditions.txt` je Lauf in `/tmp`, Rohdaten `docs/water-flow-backends-2026-11-09/*.summary.json`). Projekt `Test` selbst angelegt (das aus Thema 235 existiert nicht mehr).
Einzelne oder zwei Wiederholungen, keine drei: Richtwerte, keine Statistik.

| Zelle (Metal) | Ganzer Frame GPU p50 | `Scene`-Pass p50 | Bezug 21.10. |
|---|---|---|---|
| kein Wasser, near3 | 7,09 | 0,53 | 7,72 ganzer Frame |
| Standwasser, near3 (2 Läufe) | 16,17 / 16,50 | 10,52 / 10,74 | 17,84 ganzer Frame, Scene 11,4 |
| Flow-Variante, near3 | 16,36 | 10,57 | = Standwasser |
| kein Wasser, down3 | 8,05 | 0,21 | 8,15 |
| Standwasser, down3 | 15,93 | 11,26 | 17,18 |
| Flow-Variante, down3 (2 Läufe) | 16,28 / 16,15 | 11,67 / 11,52 | +0,3 ms `Scene` |

* **Das Standwasser hat sich nicht verschlechtert** (`Water.hasset` mit `Flow`-Switch AUS hat 14 Slots wie vorher, md5 `a2871829f267` im Lauf, Datei `4ab9b09f…`): `Scene` 10,5 bis 10,7 ms gegen 11,4 damals (Messung damals bei gesperrtem Display, heute wach).
* **Die Flow-Variante kostet höchstens +0,4 ms** im schlimmsten Fall (Wasser auf jedem Pixel, 4,3 MP). Sie ist eine Material-Variante je Körper, nur Körper mit `|flow| >= 0,02 m/s` zahlen sie.
* **Wasserfall-Bänder** sind wenige hundert Dreiecke je Band und ein Aufbau je Strich-Tick; hier **nicht gemessen** (kein Szenen-Zeuge mit Kaskade in der Perf-Harness: die Kaskade entsteht nur im Dump-Modus `HE_DUMP_WATERLAKE=cascade`, nicht in einer `.hescene`).
* **OpenGL** (`he_perf_capture.py --rhi OpenGL --no-counters`, near3, nur ganzer Frame): kein Wasser 191,5 FPS (5,2 ms), Standwasser 133,2 und 138,7 FPS (7,2 bis 7,5 ms), Flow 138,8 und 135,4 FPS (7,2 bis 7,4 ms). Wasser +2,1 ms (am 21.10.: +3,5 ms), Flow = Standwasser.

## Fallen dieses Laufs

1. **Der erste Lauf einer Zelle nach einem Szenenwechsel ist zu billig** (Standwasser near3 7,11 ms = ohne Wasser; Flow near3 r1 13,55 gegen 16,36 im zweiten Lauf; auf OpenGL lief der allererste Start in den 300-s-Timeout): Pipeline/Mesh noch nicht fertig, das Wasser wird teilweise gar nicht gezeichnet. Wiederholung nehmen und den `Scene`-Pass gegen "kein Wasser" prüfen, nie den ersten Lauf allein.
2. `water_matrix.py` braucht `$HE235_ROOT/proj/Test/Test.heproj` MIT Ordner `Content/` (`startupScene`); der Editor schreibt die `.heproj` beim ersten Lauf um.
3. Die Flow-Variante in der Perf-Harness: `physics.flow` per JSON-Patch auf den Körper der `plain_lake`-Szene (`"physics": {"flow": [2, 0]}`); die Wahl der Variante je Körper ist nur an `|flow|` gebunden.

## Handoff

- **Topic / step:** 232, Backend-Abnahme. Branch `claude/wasser-tool-str-mungen-und-automatische-wasserf-lle-an-steig`.
- **Done:** Build, `ctest` 365/365, Kaskaden-Bild Metal und OpenGL, Perf-Messung Metal (Standwasser, Flow-Variante) und OpenGL (ganzer Frame).
- **Verified:** alles in der Tabelle oben für Metal und OpenGL, mit eigenen Augen gesehen (Bilder gelesen, `ctest`-Ausgabe, `summary.json`).
- **Not verified:** **D3D11, D3D12, Vulkan (NN-WS03 offline)**; Perf der Wasserfall-Bänder; Editor-UI-Bedienung (Häkchen "Flow From Slope" / "Terraces on Slopes" nur kompiliert); Release-Pak-Spiel; Richtung der Strömung per Bildregistrierung; Wasserfall mit eigenem Gischt-Material (nicht gebaut).
- **Decisions:** keine neuen; Zahlen sind Richtwerte aus 1 bis 2 Läufen.
- **Next steps:** 1. Sobald NN-WS03 da ist: Schritt wiederholen für D3D11/D3D12/Vulkan (`HE_DUMP_RHI=...`, gleicher Befehl, `ctest`). 2. Handbuch-Eintrag zu Strömung/Terrassen/Wasserfall. 3. Optional Fall-Material mit Gischt.
